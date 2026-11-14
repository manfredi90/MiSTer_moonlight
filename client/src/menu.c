/*
 * Menu a schermo (canvas 320x240, mostrato x2 nell'overlay del core H264): elenco dei
 * PC (aggiungi, rinomina, cambia indirizzo, rimuovi; tastiera a schermo), elenco delle
 * app di Sunshine, impostazioni, abbinamento col PIN a schermo. Si naviga col pad
 * (croce, A conferma, B indietro) o con la tastiera (frecce, Invio, Esc; nei campi di
 * testo si scrive direttamente). A fine stream (combinazione di uscita o chiusura dal
 * PC) si torna qui.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */
#define _GNU_SOURCE
#include "options.h"
#include "font.h"
#include "core.h"
#include "input.h"
#include "netdiag.h"

#include <Limelight.h>
#include <client.h>
#include <errors.h>

#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define W 320
#define H 240
#define X0 16          /* margini contro l'overscan del CRT */
#define Y0 12
#define ROW_H 10
#define LIST_Y 34
#define LIST_ROWS 17
#define COLS ((W - 2 * X0) / 8)

#define C_BG     0x0C1424
#define C_HEAD   0x5AA0FF
#define C_TEXT   0xE0E4EA
#define C_DIM    0x8090A8
#define C_BAR    0x1E4C9A
#define C_VALUE  0xFFD060
#define C_OK     0x60E080
#define C_ERR    0xFF7060
#define C_LINE   0x2A3A58

static uint32_t canvas[W * H];

/* ---------- disegno ---------- */

static void fill(int x, int y, int w, int h, uint32_t c) {
  for (int j = y < 0 ? 0 : y; j < y + h && j < H; j++)
    for (int i = x < 0 ? 0 : x; i < x + w && i < W; i++)
      canvas[j * W + i] = c;
}

/* Converte in ASCII le lettere accentate più comuni (il font dell'OSD è ASCII) */
static void ascii_fold(const char *in, char *out, size_t n) {
  size_t o = 0;
  for (const unsigned char *p = (const unsigned char *)in; *p && o + 1 < n; p++) {
    if (*p < 0x80) { out[o++] = *p; continue; }
    if (*p == 0xC3 && p[1]) {
      unsigned char c = *++p;
      const char *map = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
      out[o++] = (c >= 0x80 && c <= 0xBF) ? map[c - 0x80] : '?';
      continue;
    }
    while ((p[1] & 0xC0) == 0x80) p++; /* altri caratteri multibyte: salta */
    out[o++] = '?';
  }
  out[o] = 0;
}

static void text_scaled(int x, int y, const char *s, uint32_t c, int scale) {
  char buf[256];
  ascii_fold(s, buf, sizeof(buf));
  for (const unsigned char *p = (const unsigned char *)buf; *p; p++, x += 8 * scale) {
    const unsigned char *g = mg_font[*p];
    for (int cx = 0; cx < 8; cx++)
      for (int cy = 0; cy < 8; cy++)
        if ((g[cx] >> cy) & 1)
          fill(x + cx * scale, y + cy * scale, scale, scale, c);
  }
}

static void text(int x, int y, const char *s, uint32_t c) { text_scaled(x, y, s, c, 1); }

static int text_len(const char *s) {
  char buf[256];
  ascii_fold(s, buf, sizeof(buf));
  return (int)strlen(buf);
}

static void text_right(int xr, int y, const char *s, uint32_t c) { text(xr - text_len(s) * 8, y, s, c); }
static void text_center(int y, const char *s, uint32_t c, int scale) {
  text_scaled((W - text_len(s) * 8 * scale) / 2, y, s, c, scale);
}

/* Mostra il canvas nell'overlay del core */
static void present(void) { mg_core_overlay_present(canvas, W, H); }

/* ---------- stato ---------- */

typedef struct { char name[64]; int id; } app_entry;

enum { ST_CONNECT, ST_PAIR, ST_MAIN, ST_MESSAGE, ST_PCS, ST_PCOPT, ST_EDIT };
enum { IT_APP, IT_HEADER, IT_BITRATE, IT_AUDIO, IT_QUITAFTER,
       IT_QUITAPP, IT_REFRESH, IT_EXIT, IT_PCS };

/* i PC con Sunshine (salvati in menu.cfg come pc=indirizzo|nome) */
#define MAX_PCS 16
typedef struct { char host[64]; char name[32]; } pc_entry;
static pc_entry pcs[MAX_PCS];
static int n_pcs, cur_pc = -1;
static bool pcs_saved;           /* menu.cfg ha già un elenco (anche vuoto): HOST non si riaggiunge */
static char last_pc[64];
static int pc_cursor;            /* nell'elenco dei PC: 0..n_pcs-1 i PC, poi Aggiungi, Esci */
static int opt_cursor;           /* nelle opzioni di un PC */
static bool opt_confirm;         /* "Rimuovi" premuto una volta: si chiede conferma */

/* la tastiera a schermo */
enum { ED_NEW_HOST, ED_NEW_NAME, ED_HOST, ED_NAME };
static int ed_kind, ed_pc;
static char ed_buf[64], ed_title[48], ed_new_host[64];
static int ed_row, ed_col;
static bool ed_shift;
static char ed_err[64];

typedef struct { int type; int app; } item;

static options *opt;
static char host_buf[64];        /* l'indirizzo del PC scelto (opt->host vi punta) */
static const char *keydir;
static SERVER_DATA server;
static bool server_ok;
static app_entry apps[48];
static int n_apps;
static int state = ST_CONNECT;
static item items[80];
static int n_items, cursor, scroll;
static char pin[5];
static char msg_title[64], msg_text[2][320];
static bool msg_retry;           /* nel messaggio, A riprova la connessione */
static char status[160];
static uint32_t status_color;
static double status_until;
static char last_app[64];
static char cfg_path[PATH_MAX];

static const int bitrates[] = { 0, 3000, 5000, 8000, 10000, 12000, 15000, 20000 };
#define N_BITRATES (int)(sizeof(bitrates) / sizeof(bitrates[0]))

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void set_status(const char *s, uint32_t color) {
  snprintf(status, sizeof(status), "%s", s);
  status_color = color;
  status_until = now_s() + 6;
}

static void show_message(const char *title, const char *l1, const char *l2, bool retry) {
  snprintf(msg_title, sizeof(msg_title), "%s", title);
  snprintf(msg_text[0], sizeof(msg_text[0]), "%s", l1 ? l1 : "");
  snprintf(msg_text[1], sizeof(msg_text[1]), "%s", l2 ? l2 : "");
  msg_retry = retry;
  state = ST_MESSAGE;
}

/* ---------- impostazioni salvate ---------- */

static void cfg_load(void) {
  FILE *f = fopen(cfg_path, "r");
  if (!f) return;
  char line[160];
  while (fgets(line, sizeof(line), f)) {
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;
    char *v = eq + 1;
    v[strcspn(v, "\r\n")] = 0;
    /* res= e fps= (versioni fino alla 0.1.5) si ignorano: dal menu sempre 640x480 a 60 */
    if (!strcmp(line, "bitrate")) opt->bitrate = atoi(v);
    else if (!strcmp(line, "audio")) opt->audio = atoi(v) != 0;
    else if (!strcmp(line, "quitafter")) opt->quit_after = atoi(v) != 0;
    else if (!strcmp(line, "lastapp")) snprintf(last_app, sizeof(last_app), "%s", v);
    else if (!strcmp(line, "lastpc")) snprintf(last_pc, sizeof(last_pc), "%s", v);
    else if (!strcmp(line, "pcs")) pcs_saved = true;
    else if (!strcmp(line, "pc") && n_pcs < MAX_PCS) {
      char *bar = strchr(v, '|');
      if (bar) *bar = 0;
      if (!v[0]) continue;
      snprintf(pcs[n_pcs].host, sizeof(pcs[n_pcs].host), "%s", v);
      snprintf(pcs[n_pcs].name, sizeof(pcs[n_pcs].name), "%s", bar && bar[1] ? bar + 1 : v);
      n_pcs++;
    }
  }
  fclose(f);
}

static void cfg_save(void) {
  FILE *f = fopen(cfg_path, "w");
  if (!f) return;
  fprintf(f, "bitrate=%d\naudio=%d\nquitafter=%d\nlastapp=%s\nlastpc=%s\npcs=%d\n",
          opt->bitrate, opt->audio, opt->quit_after, last_app, last_pc, n_pcs);
  for (int i = 0; i < n_pcs; i++)
    fprintf(f, "pc=%s|%s\n", pcs[i].host, pcs[i].name);
  fclose(f);
  pcs_saved = true;
}

/* ---------- lavori in background (rete) ---------- */

enum { JOB_NONE, JOB_CONNECT, JOB_PAIR };
static pthread_t job_thread;
static volatile int job, job_done;
static int job_result;
static char job_error[160];
static bool was_paired; /* già visto abbinato in questa esecuzione (col PC scelto) */
/* Un lavoro alla volta (libgamestream ha un solo handle curl). Cambiando PC mentre un lavoro
 * è in corso (un abbinamento può attendere il PIN fino a 60 s) il suo esito si scarta: conta
 * solo quello col numero di PC attuale; la connessione al nuovo PC parte quando finisce. */
static int pc_gen, job_gen;
static bool connect_pending;
static char job_host[64];

static void *job_main(void *arg) {
  int ret = GS_OK;
  job_error[0] = 0;
  if (job == JOB_PAIR) {
    ret = gs_pair(&server, pin);
    if (ret != GS_OK)
      snprintf(job_error, sizeof(job_error), "%s", gs_error ? gs_error : "errore sconosciuto");
  } else {
    double t0 = now_s();
    ret = gs_init(&server, job_host, 47989, keydir, 0, true);
    /* All'avvio la rete del MiSTer può non essere ancora pronta (il caricamento
     * del core e il Wi-Fi appena riconfigurato): qualche tentativo prima di
     * mostrare l'errore. */
    for (int i = 0; i < 4 && ret != GS_OK && !mg_exiting() && job_gen == pc_gen; i++) {
      printf("[menu] connessione fallita dopo %.1f s (%s): riprovo (%d)\n", now_s() - t0,
             gs_error ? gs_error : "?", i + 1);
      if (i == 0)
        mg_net_diag("connessione a Sunshine fallita", job_host);
      sleep(2);
      t0 = now_s();
      ret = gs_init(&server, job_host, 47989, keydir, 0, true);
    }
    if (ret == GS_OK && !server.paired)
      printf("[menu] serverinfo: non abbinato secondo HTTP (app in corso %d); HTTPS fallito dopo %.1f s: %s\n",
             server.currentGame, now_s() - t0, gs_https_error[0] ? gs_https_error : "?");
    /* Subito dopo uno stream Sunshine può non rispondere sulla porta HTTPS:
     * libgamestream ripiega allora su HTTP, che dice sempre "non abbinato".
     * Se l'abbinamento c'era, si riprova qualche volta prima di chiedere il PIN. */
    for (int i = 0; i < 5 && ret == GS_OK && !server.paired && was_paired && !mg_exiting() && job_gen == pc_gen; i++) {
      printf("[menu] Sunshine risponde \"non abbinato\": riprovo (%d)\n", i + 1);
      sleep(2);
      t0 = now_s();
      ret = gs_init(&server, job_host, 47989, keydir, 0, true);
      if (ret == GS_OK && !server.paired)
        printf("[menu]   ancora no: HTTPS fallito dopo %.1f s: %s\n", now_s() - t0, gs_https_error);
    }
    if (ret == GS_OK && server.paired && job_gen == pc_gen)
      was_paired = true;
    if (ret != GS_OK) {
      snprintf(job_error, sizeof(job_error), "%s", gs_error ? gs_error : "nessuna risposta");
    } else if (server.paired && job_gen == pc_gen) {
      PAPP_LIST list = NULL;
      if (gs_applist(&server, &list) == GS_OK) {
        /* libgamestream costruisce l'elenco al contrario: lo rigiriamo */
        int n = 0;
        for (PAPP_LIST l = list; l; l = l->next) n++;
        if (n > 48) n = 48;
        n_apps = n;
        for (PAPP_LIST l = list; l && n > 0; l = l->next) {
          n--;
          snprintf(apps[n].name, sizeof(apps[n].name), "%s", l->name);
          apps[n].id = l->id;
        }
      } else {
        ret = GS_FAILED;
        snprintf(job_error, sizeof(job_error), "elenco app: %s", gs_error ? gs_error : "errore");
      }
    }
  }
  job_result = ret;
  job_done = 1;
  return NULL;
}

static void start_job(int j) {
  if (job != JOB_NONE) {
    if (j == JOB_CONNECT && job_gen != pc_gen) connect_pending = true;   /* dopo quello vecchio */
    return;
  }
  job = j;
  job_done = 0;
  job_gen = pc_gen;
  snprintf(job_host, sizeof(job_host), "%s", host_buf);
  pthread_create(&job_thread, NULL, job_main, NULL);
}

static void rebuild_items(void);

static void finish_job(void) {
  if (job == JOB_NONE || !job_done) return;
  pthread_join(job_thread, NULL);
  int j = job;
  job = JOB_NONE;
  if (job_gen != pc_gen) {                   /* il PC è cambiato: l'esito non conta */
    printf("[menu] esito per %s scartato (PC cambiato)\n", job_host);
    if (connect_pending) { connect_pending = false; start_job(JOB_CONNECT); }
    return;
  }
  if (j == JOB_CONNECT) {
    if (job_result != GS_OK) {
      char l1[160];
      snprintf(l1, sizeof(l1), "Sunshine non risponde su %s", host_buf);
      server_ok = false;
      if (state != ST_MESSAGE)
        show_message("Connessione", l1, job_error, true);
    } else if (!server.paired && gs_https_error[0] && (was_paired || server.currentGame != 0)) {
      /* Con un'app aperta Sunshine rifiuterebbe comunque l'abbinamento
       * ("The computer is currently in a game"): il problema è la connessione
       * HTTPS, non l'abbinamento. */
      char l2[320];
      snprintf(l2, sizeof(l2), "Errore: %s. Se il problema resta, chiudi l'app sul PC (in Sunshine: Risoluzione dei problemi, Forza chiusura) e riprova.", gs_https_error);
      server_ok = false;
      show_message("Connessione", "Sunshine non risponde sulla connessione sicura", l2, true);
    } else if (!server.paired) {
      srandom(getpid() ^ (unsigned)time(NULL));
      snprintf(pin, sizeof(pin), "%04ld", random() % 10000);
      state = ST_PAIR;
      start_job(JOB_PAIR);
    } else {
      server_ok = true;
      rebuild_items();
      if (state == ST_CONNECT) state = ST_MAIN; /* un messaggio aperto resta finché non si preme A */
    }
  } else if (j == JOB_PAIR) {
    if (job_result == GS_OK) {
      set_status("Abbinamento riuscito", C_OK);
      state = ST_CONNECT;
      start_job(JOB_CONNECT);
    } else {
      show_message("Abbinamento fallito", job_error, "", true);
    }
  }
}

/* ---------- elenco ---------- */

static void rebuild_items(void) {
  int old_type = n_items ? items[cursor].type : -1;
  int old_app = n_items ? items[cursor].app : -1;
  n_items = 0;
  for (int i = 0; i < n_apps; i++)
    items[n_items++] = (item){ IT_APP, i };
  if (!n_apps)
    items[n_items++] = (item){ IT_HEADER, -2 }; /* "nessuna app" */
  items[n_items++] = (item){ IT_HEADER, -1 };
  items[n_items++] = (item){ IT_BITRATE, -1 };
  items[n_items++] = (item){ IT_AUDIO, -1 };
  items[n_items++] = (item){ IT_QUITAFTER, -1 };
  if (server.currentGame)
    items[n_items++] = (item){ IT_QUITAPP, -1 };
  items[n_items++] = (item){ IT_REFRESH, -1 };
  items[n_items++] = (item){ IT_PCS, -1 };
  items[n_items++] = (item){ IT_EXIT, -1 };

  /* rimette il cursore dov'era, oppure sull'ultima app usata */
  cursor = 0;
  for (int i = 0; i < n_items; i++) {
    if (old_type >= 0 && items[i].type == old_type && items[i].app == old_app) { cursor = i; break; }
    if (old_type < 0 && items[i].type == IT_APP && !strcmp(apps[items[i].app].name, last_app)) { cursor = i; break; }
  }
  if (items[cursor].type == IT_HEADER)
    cursor++;
}

static void change_setting(int type, int dir) {
  switch (type) {
  case IT_BITRATE: {
    int i = 0;
    while (i < N_BITRATES && bitrates[i] != opt->bitrate) i++;
    if (i == N_BITRATES) i = 0;
    opt->bitrate = bitrates[(i + dir + N_BITRATES) % N_BITRATES];
    break;
  }
  case IT_AUDIO: opt->audio = !opt->audio; break;
  case IT_QUITAFTER: opt->quit_after = !opt->quit_after; break;
  default: return;
  }
  cfg_save();
}

static void item_label(const item *it, char *label, size_t ln, char *value, size_t vn) {
  value[0] = 0;
  switch (it->type) {
  case IT_APP:
    snprintf(label, ln, "%s", apps[it->app].name);
    if (server.currentGame == apps[it->app].id) snprintf(value, vn, "in corso");
    break;
  case IT_HEADER: snprintf(label, ln, "%s", it->app == -2 ? "(nessuna app in Sunshine)" : ""); break;
  case IT_BITRATE:
    snprintf(label, ln, "Bitrate");
    if (opt->bitrate) snprintf(value, vn, "< %d kbps >", opt->bitrate);
    else snprintf(value, vn, "< auto >");
    break;
  case IT_AUDIO: snprintf(label, ln, "Audio"); snprintf(value, vn, "< %s >", opt->audio ? "si" : "no"); break;
  case IT_QUITAFTER: snprintf(label, ln, "Chiudi l'app all'uscita"); snprintf(value, vn, "< %s >", opt->quit_after ? "si" : "no"); break;
  case IT_QUITAPP: snprintf(label, ln, "Chiudi l'app aperta sul PC"); break;
  case IT_REFRESH: snprintf(label, ln, "Aggiorna elenco"); break;
  case IT_PCS: snprintf(label, ln, "Cambia PC"); break;
  case IT_EXIT: snprintf(label, ln, "Esci"); break;
  }
}

/* ---------- schermate ---------- */

static void draw_frame(const char *footer) {
  fill(0, 0, W, H, C_BG);
  text(X0, Y0, "MOONLIGHT", C_HEAD);
  const char *who = state == ST_PCS || state == ST_EDIT ? "PC con Sunshine"
                  : state == ST_PCOPT && ed_pc >= 0 && ed_pc < n_pcs ? pcs[ed_pc].host
                  : cur_pc >= 0 && cur_pc < n_pcs ? pcs[cur_pc].name : host_buf;
  char wb[40];
  snprintf(wb, sizeof(wb), "%.*s", COLS - 11, who);
  text_right(W - X0, Y0, wb, C_DIM);
  fill(X0, Y0 + 12, W - 2 * X0, 1, C_LINE);
  fill(X0, H - Y0 - 12, W - 2 * X0, 1, C_LINE);
  if (status[0] && now_s() < status_until)
    text(X0, H - Y0 - 24, status, status_color);
  if (footer)
    text(X0, H - Y0 - 8, footer, C_DIM);
}

static void draw_main(void) {
  draw_frame("A avvia/ok  < > cambia  B PC");
  if (cursor < scroll) scroll = cursor;
  if (cursor >= scroll + LIST_ROWS) scroll = cursor - LIST_ROWS + 1;
  for (int r = 0; r < LIST_ROWS && scroll + r < n_items; r++) {
    const item *it = &items[scroll + r];
    int y = LIST_Y + r * ROW_H;
    char label[80], value[64];
    item_label(it, label, sizeof(label), value, sizeof(value));
    if (it->type == IT_HEADER) {
      if (it->app == -1) {
        fill(X0, y + 4, W - 2 * X0, 1, C_LINE);
        fill(X0 + 8, y, 14 * 8, 8, C_BG);
        text(X0 + 8, y, " Impostazioni ", C_DIM);
      } else {
        text(X0 + 8, y, label, C_DIM);
      }
      continue;
    }
    bool sel = scroll + r == cursor;
    if (sel) fill(X0 - 4, y - 1, W - 2 * X0 + 8, ROW_H, C_BAR);
    /* nome troncato per lasciare spazio al valore */
    int maxc = COLS - 1 - (value[0] ? text_len(value) + 1 : 0);
    if (text_len(label) > maxc && maxc > 3) { label[maxc - 1] = '.'; label[maxc] = 0; }
    text(X0 + 4, y, label, sel ? 0xFFFFFF : C_TEXT);
    if (value[0])
      text_right(W - X0 - 4, y, value, it->type == IT_APP ? C_OK : C_VALUE);
  }
  if (scroll > 0) text_right(W - X0, LIST_Y - 10, "^", C_DIM);
  if (scroll + LIST_ROWS < n_items) text_right(W - X0, LIST_Y + LIST_ROWS * ROW_H - 2, "v", C_DIM);
}

static void draw_connect(void) {
  draw_frame("B indietro");
  char l[96];
  if (job != JOB_NONE && job_gen != pc_gen)
    snprintf(l, sizeof(l), "Attendo la fine della richiesta precedente...");
  else if (cur_pc >= 0 && cur_pc < n_pcs && strcmp(pcs[cur_pc].name, host_buf))
    snprintf(l, sizeof(l), "Connessione a %s (%s)...", pcs[cur_pc].name, host_buf);
  else
    snprintf(l, sizeof(l), "Connessione a %s...", host_buf);
  text_center(100, l, C_TEXT, 1);
  static const char spin[] = "|/-\\";
  char s[2] = { spin[(int)(now_s() * 6) % 4], 0 };
  text_center(120, s, C_HEAD, 2);
}

static void draw_pair(void) {
  draw_frame("B indietro");
  text_center(46, "Abbinamento con Sunshine", C_HEAD, 1);
  text_center(70, "Sul PC apri la pagina web di Sunshine", C_TEXT, 1);
  text_center(82, "(https://localhost:47990), scheda PIN,", C_TEXT, 1);
  text_center(94, "e inserisci questo codice:", C_TEXT, 1);
  text_center(116, pin, C_VALUE, 4);
  text_center(162, "In attesa...", C_DIM, 1);
}

/* Scrive un testo centrato andando a capo sugli spazi; ritorna la y successiva */
static int text_wrapped(int y, const char *s, uint32_t c) {
  char buf[256];
  ascii_fold(s, buf, sizeof(buf));
  const int maxc = COLS;
  char *p = buf;
  while (*p) {
    while (*p == ' ') p++;
    int n = (int)strlen(p);
    if (n > maxc) {
      n = maxc;
      while (n > 0 && p[n] != ' ') n--;
      if (n == 0) n = maxc;
    }
    char line[64];
    snprintf(line, sizeof(line), "%.*s", n, p);
    text_center(y, line, c, 1);
    y += ROW_H + 2;
    p += n;
  }
  return y;
}

static void draw_message(void) {
  draw_frame(msg_retry ? "A riprova  B indietro" : "A ok");
  text_center(76, msg_title, C_ERR, 1);
  int y = text_wrapped(100, msg_text[0], C_TEXT);
  text_wrapped(y + 4, msg_text[1], C_DIM);
}

static void draw_starting(const char *name) {
  draw_frame(NULL);
  char l[96];
  snprintf(l, sizeof(l), "Avvio di %s", name);
  text_center(104, l, C_TEXT, 1);
  char r[64];
  int ri = mg_res_find(opt->width, opt->height);
  if (ri >= 0) snprintf(r, sizeof(r), "%s", mg_res_at(ri)->desc);
  else snprintf(r, sizeof(r), "%dx%d", opt->width, opt->height);
  text_center(120, r, C_DIM, 1);
}

/* ---------- elenco dei PC ---------- */

static void pick_pc(int i);

static void draw_pcs(void) {
  draw_frame("A connetti  > opzioni  B esci");
  int n = n_pcs + 2;
  int top = pc_cursor - LIST_ROWS + 1 > 0 ? pc_cursor - LIST_ROWS + 1 : 0;
  for (int r = 0; r < LIST_ROWS && top + r < n; r++) {
    int i = top + r;
    int y = LIST_Y + r * ROW_H;
    bool sel = i == pc_cursor;
    if (sel) fill(X0 - 4, y - 1, W - 2 * X0 + 8, ROW_H, C_BAR);
    if (i < n_pcs) {
      char lab[40], val[64];
      int maxv = COLS / 2;
      snprintf(val, sizeof(val), "%.*s", maxv, pcs[i].host);
      snprintf(lab, sizeof(lab), "%.*s", COLS - 2 - text_len(val), pcs[i].name);
      text(X0 + 4, y, lab, sel ? 0xFFFFFF : C_TEXT);
      text_right(W - X0 - 4, y, val, C_DIM);
    } else if (i == n_pcs) {
      text(X0 + 4, y, "+ Aggiungi PC", sel ? 0xFFFFFF : C_OK);
    } else {
      text(X0 + 4, y, "Esci", sel ? 0xFFFFFF : C_TEXT);
    }
  }
  if (!n_pcs)
    text_wrapped(LIST_Y + 4 * ROW_H, "Aggiungi il PC con Sunshine scrivendo il suo indirizzo IP (lo trovi in Sunshine o con ipconfig sul PC).", C_DIM);
}

static const char *pcopt_labels[] = { "Connetti", "Rinomina", "Cambia indirizzo", "Rimuovi", "Indietro" };
#define N_PCOPT 5

static void draw_pcopt(void) {
  draw_frame("A ok  B indietro");
  char t[64];
  snprintf(t, sizeof(t), "%.36s", pcs[ed_pc].name);
  text_center(LIST_Y, t, C_HEAD, 1);
  for (int i = 0; i < N_PCOPT; i++) {
    int y = LIST_Y + 20 + i * (ROW_H + 4);
    bool sel = i == opt_cursor;
    if (sel) fill(X0 + 60, y - 2, W - 2 * X0 - 120, ROW_H + 2, C_BAR);
    const char *l = i == 3 && opt_confirm ? "Conferma: rimuovi" : pcopt_labels[i];
    text_center(y, l, i == 3 && opt_confirm ? C_ERR : sel ? 0xFFFFFF : C_TEXT, 1);
  }
}

/* la tastiera: 4 righe di 10 caratteri e una di tasti speciali */
static const char *kb_rows[2][4] = {
  { "1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm._:" },
  { "1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM._:" },
};
static const char *kb_special[] = { "Maiusc", "Spazio", "Canc", "OK" };
#define KB_CW 24
#define KB_RH 16
#define KB_X ((W - 10 * KB_CW) / 2)
#define KB_Y 92

static void draw_edit(void) {
  draw_frame("A scrivi  Start conferma  B annulla");
  text_center(LIST_Y, ed_title, C_HEAD, 1);
  /* il campo */
  int fw = (COLS - 2) * 8;
  fill((W - fw) / 2 - 4, LIST_Y + 18, fw + 8, 14, C_LINE);
  char shown[64];
  int maxc = COLS - 3;
  int len = (int)strlen(ed_buf);
  snprintf(shown, sizeof(shown), "%s%s", len > maxc ? ed_buf + len - maxc : ed_buf,
           ((int)(now_s() * 2) & 1) ? "_" : " ");
  text((W - fw) / 2, LIST_Y + 21, shown, C_VALUE);
  if (ed_err[0])
    text_center(LIST_Y + 38, ed_err, C_ERR, 1);
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 10; c++) {
      int x = KB_X + c * KB_CW, y = KB_Y + r * KB_RH;
      bool sel = ed_row == r && ed_col == c;
      fill(x + 1, y, KB_CW - 2, KB_RH - 2, sel ? C_BAR : C_LINE);
      char ch[2] = { kb_rows[ed_shift][r][c], 0 };
      text(x + (KB_CW - 8) / 2, y + (KB_RH - 10) / 2 + 1, ch, sel ? 0xFFFFFF : C_TEXT);
    }
  int y = KB_Y + 4 * KB_RH + 2;
  int sw = 10 * KB_CW / 4;
  for (int i = 0; i < 4; i++) {
    int x = KB_X + i * sw;
    bool sel = ed_row == 4 && ed_col == i;
    uint32_t bg = sel ? C_BAR : (i == 0 && ed_shift) ? 0x3A5A30 : C_LINE;
    fill(x + 1, y, sw - 2, KB_RH - 2, bg);
    text(x + (sw - text_len(kb_special[i]) * 8) / 2, y + 4, kb_special[i], sel ? 0xFFFFFF : i == 3 ? C_OK : C_TEXT);
  }
}

static void start_edit(int kind, int pc, const char *initial) {
  ed_kind = kind;
  ed_pc = pc;
  snprintf(ed_buf, sizeof(ed_buf), "%s", initial ? initial : "");
  snprintf(ed_title, sizeof(ed_title), "%s",
           kind == ED_NEW_HOST || kind == ED_HOST ? "Indirizzo IP del PC" : "Nome del PC");
  ed_err[0] = 0;
  ed_row = 0; ed_col = 0;
  ed_shift = false;
  state = ST_EDIT;
}

static bool host_ok(const char *h) {
  if (!h[0] || strlen(h) > 63) return false;
  for (const char *p = h; *p; p++)
    if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          *p == '.' || *p == '-' || *p == '_' || *p == ':'))
      return false;
  return true;
}

static int find_pc(const char *h) {
  for (int i = 0; i < n_pcs; i++)
    if (!strcasecmp(pcs[i].host, h)) return i;
  return -1;
}

static void edit_done(void) {
  /* spazi in testa e in coda via */
  char *b = ed_buf;
  while (*b == ' ') b++;
  memmove(ed_buf, b, strlen(b) + 1);
  for (int n = (int)strlen(ed_buf); n > 0 && ed_buf[n - 1] == ' '; n--) ed_buf[n - 1] = 0;
  switch (ed_kind) {
  case ED_NEW_HOST:
  case ED_HOST: {
    if (!host_ok(ed_buf)) { snprintf(ed_err, sizeof(ed_err), "Indirizzo non valido"); return; }
    int dup = find_pc(ed_buf);
    if (dup >= 0 && dup != ed_pc) { snprintf(ed_err, sizeof(ed_err), "C'e' gia': %.40s", pcs[dup].name); return; }
    if (ed_kind == ED_NEW_HOST) {
      snprintf(ed_new_host, sizeof(ed_new_host), "%s", ed_buf);
      start_edit(ED_NEW_NAME, -1, "");
      return;
    }
    bool same_name = !strcmp(pcs[ed_pc].name, pcs[ed_pc].host);
    if (same_name) snprintf(pcs[ed_pc].name, sizeof(pcs[ed_pc].name), "%s", ed_buf);
    snprintf(pcs[ed_pc].host, sizeof(pcs[ed_pc].host), "%s", ed_buf);
    if (ed_pc == cur_pc) cur_pc = -1, server_ok = false;   /* va ricollegato */
    cfg_save();
    set_status("Indirizzo cambiato", C_OK);
    state = ST_PCOPT;
    return;
  }
  case ED_NEW_NAME: {
    if (n_pcs >= MAX_PCS) { state = ST_PCS; set_status("Troppi PC", C_ERR); return; }
    pc_entry *e = &pcs[n_pcs];
    snprintf(e->host, sizeof(e->host), "%s", ed_new_host);
    snprintf(e->name, sizeof(e->name), "%.31s", ed_buf[0] ? ed_buf : ed_new_host);
    pc_cursor = n_pcs++;
    cfg_save();
    printf("[menu] PC aggiunto: %s (%s)\n", e->name, e->host);
    pick_pc(pc_cursor);
    return;
  }
  case ED_NAME:
    snprintf(pcs[ed_pc].name, sizeof(pcs[ed_pc].name), "%.31s", ed_buf[0] ? ed_buf : pcs[ed_pc].host);
    cfg_save();
    set_status("Nome cambiato", C_OK);
    state = ST_PCOPT;
    return;
  }
}

static void edit_cancel(void) {
  state = ed_kind == ED_HOST || ed_kind == ED_NAME ? ST_PCOPT : ST_PCS;
}

static void edit_type(int c) {
  size_t n = strlen(ed_buf);
  ed_err[0] = 0;
  if (n + 1 < sizeof(ed_buf) && (ed_kind == ED_NAME || ed_kind == ED_NEW_NAME ? n < 31 : 1))
    ed_buf[n] = (char)c, ed_buf[n + 1] = 0;
}

static void edit_key(int k) {
  if (k >= MG_KEY_CHAR) { edit_type(k - MG_KEY_CHAR); return; }
  switch (k) {
  case MG_KEY_UP:
  case MG_KEY_DOWN: {
    int r = (ed_row + (k == MG_KEY_UP ? 4 : 1)) % 5;
    if (ed_row == 4) ed_col = ed_col * 10 / 4 + 1;   /* dai tasti speciali alle lettere */
    else if (r == 4) ed_col = ed_col * 4 / 10;       /* e viceversa */
    ed_row = r;
    break;
  }
  case MG_KEY_LEFT: ed_col = ed_row == 4 ? (ed_col + 3) % 4 : (ed_col + 9) % 10; break;
  case MG_KEY_RIGHT: ed_col = ed_row == 4 ? (ed_col + 1) % 4 : (ed_col + 1) % 10; break;
  case MG_KEY_DEL: { size_t n = strlen(ed_buf); if (n) ed_buf[n - 1] = 0; ed_err[0] = 0; break; }
  case MG_KEY_BACK: edit_cancel(); break;
  case MG_KEY_ENTER: edit_done(); break;
  case MG_KEY_OK:
    if (ed_row < 4) edit_type(kb_rows[ed_shift][ed_row][ed_col]);
    else if (ed_col == 0) ed_shift = !ed_shift;
    else if (ed_col == 1) edit_type(' ');
    else if (ed_col == 2) { size_t n = strlen(ed_buf); if (n) ed_buf[n - 1] = 0; ed_err[0] = 0; }
    else edit_done();
    break;
  }
  if (ed_row == 4 && ed_col > 3) ed_col = 3;
  if (ed_row < 4 && ed_col > 9) ed_col = 9;
}

/* sceglie il PC i e si collega */
static void pick_pc(int i) {
  if (i != cur_pc || !server_ok) {
    pc_gen++;
    was_paired = false;
    server_ok = false;
    n_apps = 0;
    n_items = 0;
    cur_pc = i;
    snprintf(host_buf, sizeof(host_buf), "%s", pcs[i].host);
    opt->host = host_buf;
    snprintf(last_pc, sizeof(last_pc), "%s", pcs[i].host);
    cfg_save();
    printf("[menu] PC scelto: %s (%s)\n", pcs[i].name, pcs[i].host);
    state = ST_CONNECT;
    start_job(JOB_CONNECT);
  } else {
    state = ST_MAIN;                         /* già collegato: dritti alle app */
  }
}

static void back_to_pcs(void) {
  state = ST_PCS;
  if (cur_pc >= 0) pc_cursor = cur_pc;
}

static int render_logged;
static void render(void) {
  int logged = render_logged;
  if (logged < 3) render_logged++;
  if (logged < 3) printf("[menu] disegno schermata %d\n", state);
  switch (state) {
  case ST_CONNECT: draw_connect(); break;
  case ST_PAIR: draw_pair(); break;
  case ST_MAIN: draw_main(); break;
  case ST_MESSAGE: draw_message(); break;
  case ST_PCS: draw_pcs(); break;
  case ST_PCOPT: draw_pcopt(); break;
  case ST_EDIT: draw_edit(); break;
  }
  present();
}

/* ---------- azioni ---------- */

static void launch(int app) {
  snprintf(last_app, sizeof(last_app), "%s", apps[app].name);
  cfg_save();
  draw_starting(apps[app].name);
  present();
  char msg[200];
  printf("[menu] avvio %s\n", apps[app].name);
  int r = mg_stream_session(&server, opt, apps[app].id, msg, sizeof(msg));
  if (mg_exiting()) return;
  printf("[menu] ritorno al menu\n");
  render_logged = 0;
  mg_input_set_mode(MG_INPUT_MENU);
  if (r != 0) {
    show_message("Avvio non riuscito", msg, "", false);
  } else {
    state = ST_MAIN;
    if (msg[0]) set_status(msg, C_DIM);
  }
  /* aggiorna l'app in corso e l'elenco */
  start_job(JOB_CONNECT);
}

static bool handle_key(int k) {
  if (k == MG_KEY_NONE) return true;
  if (state == ST_EDIT) { edit_key(k); return true; }
  /* fuori dai campi di testo: Backspace = indietro, Spazio = ok, le altre lettere niente */
  if (k == MG_KEY_DEL) k = MG_KEY_BACK;
  else if (k == MG_KEY_ENTER) k = MG_KEY_OK;
  else if (k == MG_KEY_CHAR + ' ') k = MG_KEY_OK;
  else if (k >= MG_KEY_CHAR) return true;
  switch (state) {
  case ST_CONNECT:
  case ST_PAIR:
    if (k == MG_KEY_BACK) { pc_gen++; server_ok = false; cur_pc = -1; back_to_pcs(); }
    break;
  case ST_MESSAGE:
    if (k == MG_KEY_OK) {
      if (msg_retry) { state = ST_CONNECT; start_job(JOB_CONNECT); }
      else state = server_ok ? ST_MAIN : ST_PCS;
    } else if (k == MG_KEY_BACK) {
      if (msg_retry || !server_ok) back_to_pcs();
      else state = ST_MAIN;
    }
    break;
  case ST_PCS: {
    int n = n_pcs + 2;
    if (pc_cursor >= n) pc_cursor = n - 1;
    if (k == MG_KEY_UP) pc_cursor = (pc_cursor + n - 1) % n;
    else if (k == MG_KEY_DOWN) pc_cursor = (pc_cursor + 1) % n;
    else if (k == MG_KEY_BACK) return false;
    else if (k == MG_KEY_RIGHT && pc_cursor < n_pcs) {
      ed_pc = pc_cursor; opt_cursor = 0; opt_confirm = false; state = ST_PCOPT;
    } else if (k == MG_KEY_OK) {
      if (pc_cursor < n_pcs) pick_pc(pc_cursor);
      else if (pc_cursor == n_pcs) {
        if (n_pcs >= MAX_PCS) set_status("Elenco pieno: rimuovi un PC", C_ERR);
        else start_edit(ED_NEW_HOST, -1, "");
      } else return false;
    }
    break;
  }
  case ST_PCOPT:
    if (k == MG_KEY_UP) { opt_cursor = (opt_cursor + N_PCOPT - 1) % N_PCOPT; opt_confirm = false; }
    else if (k == MG_KEY_DOWN) { opt_cursor = (opt_cursor + 1) % N_PCOPT; opt_confirm = false; }
    else if (k == MG_KEY_BACK || k == MG_KEY_LEFT) { state = ST_PCS; pc_cursor = ed_pc; }
    else if (k == MG_KEY_OK) {
      switch (opt_cursor) {
      case 0: pc_cursor = ed_pc; pick_pc(ed_pc); break;
      case 1: start_edit(ED_NAME, ed_pc, pcs[ed_pc].name); break;
      case 2: start_edit(ED_HOST, ed_pc, pcs[ed_pc].host); break;
      case 3:
        if (!opt_confirm) { opt_confirm = true; break; }
        printf("[menu] PC rimosso: %s (%s)\n", pcs[ed_pc].name, pcs[ed_pc].host);
        if (ed_pc == cur_pc) { cur_pc = -1; server_ok = false; pc_gen++; }
        else if (cur_pc > ed_pc) cur_pc--;
        memmove(&pcs[ed_pc], &pcs[ed_pc + 1], (size_t)(n_pcs - ed_pc - 1) * sizeof(pcs[0]));
        n_pcs--;
        cfg_save();
        set_status("PC rimosso", C_DIM);
        opt_confirm = false;
        state = ST_PCS;
        pc_cursor = ed_pc < n_pcs ? ed_pc : n_pcs;
        break;
      default: state = ST_PCS; pc_cursor = ed_pc; break;
      }
    }
    break;
  case ST_MAIN: {
    int t = items[cursor].type;
    if (k == MG_KEY_UP || k == MG_KEY_DOWN) {
      int d = k == MG_KEY_UP ? -1 : 1;
      int c = cursor;
      do { c = (c + d + n_items) % n_items; } while (items[c].type == IT_HEADER && c != cursor);
      cursor = c;
    } else if (k == MG_KEY_LEFT || k == MG_KEY_RIGHT) {
      change_setting(t, k == MG_KEY_LEFT ? -1 : 1);
    } else if (k == MG_KEY_BACK) {
      back_to_pcs();
    } else if (k == MG_KEY_OK) {
      if (t == IT_APP) {
        if (job != JOB_NONE) set_status("Attendi l'aggiornamento...", C_DIM);
        else launch(items[cursor].app);
      }
      else if (t == IT_EXIT) return false;
      else if (t == IT_PCS) back_to_pcs();
      else if (t == IT_REFRESH) { start_job(JOB_CONNECT); set_status("Aggiornamento...", C_DIM); }
      else if (t == IT_QUITAPP) {
        set_status(gs_quit_app(&server) == GS_OK ? "App chiusa sul PC" : "Chiusura non riuscita", C_DIM);
        server.currentGame = 0;
        start_job(JOB_CONNECT);
      } else change_setting(t, 1);
    }
    break;
  }
  }
  return true;
}

int mg_menu_run(options *o, const char *kd) {
  opt = o;
  keydir = kd;

  /* menu.cfg accanto all'eseguibile */
  char exe[PATH_MAX];
  ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (len > 0) {
    exe[len] = 0;
    char *slash = strrchr(exe, '/');
    if (slash) *slash = 0;
    snprintf(cfg_path, sizeof(cfg_path), "%s/menu.cfg", exe);
  } else {
    snprintf(cfg_path, sizeof(cfg_path), "menu.cfg");
  }
  if (!o->no_menu_cfg)
    cfg_load();
  /* il core è a 640 pixel e 60 Hz: dal menu sempre 640x480 a 60 immagini al secondo */
  o->width = 640;
  o->height = 480;
  o->fps = 0;

  /* l'elenco dei PC: HOST della configurazione entra la prima volta (poi si gestisce dal menu) */
  if (o->host && o->host[0] && host_ok(o->host) && find_pc(o->host) < 0 && !pcs_saved && n_pcs < MAX_PCS) {
    snprintf(pcs[n_pcs].host, sizeof(pcs[n_pcs].host), "%s", o->host);
    snprintf(pcs[n_pcs].name, sizeof(pcs[n_pcs].name), "PC");
    n_pcs++;
    if (!last_pc[0]) snprintf(last_pc, sizeof(last_pc), "%s", o->host);
    if (!o->no_menu_cfg) cfg_save();
  }
  pc_cursor = 0;
  int lp = find_pc(last_pc);
  if (lp >= 0) pc_cursor = lp;
  else if (!n_pcs) pc_cursor = 0;            /* "Aggiungi PC" */

  mg_input_start(o->pads, mg_request_quit);
  mg_input_set_mode(MG_INPUT_MENU);

  state = ST_PCS;
  double last_present = 0;
  bool dirty = true;
  while (!mg_exiting()) {
    int k = mg_input_menu_key(dirty ? 0 : 100);
    if (k != MG_KEY_NONE) dirty = true;
    if (!handle_key(k)) break;
    if (job != JOB_NONE && job_done) { finish_job(); dirty = true; }
    double t = now_s();
    /* ridisegna se serve, e comunque ogni tanto (animazione, messaggi a tempo, Groovy) */
    if (dirty || t - last_present > (state == ST_CONNECT ? 0.15 : state == ST_EDIT ? 0.5 : 1.0)) {
      render();
      last_present = t;
      dirty = false;
    }
  }
  /* un eventuale lavoro di rete in corso finirà da solo: non lo aspettiamo */
  mg_input_stop();
  mg_core_overlay_hide();
  return 0;
}
