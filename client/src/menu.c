/*
 * Menu a schermo (canvas 320x240, mostrato x2 nell'overlay del core H264): elenco
 * delle app di Sunshine, impostazioni, abbinamento col PIN a schermo. Si naviga col
 * pad (croce, A conferma, B indietro) o con la tastiera (frecce, Invio, Esc).
 * A fine stream (combinazione di uscita o chiusura dal PC) si torna qui.
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

enum { ST_CONNECT, ST_PAIR, ST_MAIN, ST_MESSAGE };
enum { IT_APP, IT_HEADER, IT_BITRATE, IT_AUDIO, IT_QUITAFTER,
       IT_QUITAPP, IT_REFRESH, IT_EXIT };

typedef struct { int type; int app; } item;

static options *opt;
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
  }
  fclose(f);
}

static void cfg_save(void) {
  FILE *f = fopen(cfg_path, "w");
  if (!f) return;
  fprintf(f, "bitrate=%d\naudio=%d\nquitafter=%d\nlastapp=%s\n",
          opt->bitrate, opt->audio, opt->quit_after, last_app);
  fclose(f);
}

/* ---------- lavori in background (rete) ---------- */

enum { JOB_NONE, JOB_CONNECT, JOB_PAIR };
static pthread_t job_thread;
static volatile int job, job_done;
static int job_result;
static char job_error[160];
static bool was_paired; /* già visto abbinato in questa esecuzione */

static void *job_main(void *arg) {
  int ret = GS_OK;
  job_error[0] = 0;
  if (job == JOB_PAIR) {
    ret = gs_pair(&server, pin);
    if (ret != GS_OK)
      snprintf(job_error, sizeof(job_error), "%s", gs_error ? gs_error : "errore sconosciuto");
  } else {
    double t0 = now_s();
    ret = gs_init(&server, opt->host, 47989, keydir, 0, true);
    /* All'avvio la rete del MiSTer può non essere ancora pronta (il caricamento
     * del core e il Wi-Fi appena riconfigurato): qualche tentativo prima di
     * mostrare l'errore. */
    for (int i = 0; i < 4 && ret != GS_OK && !mg_exiting(); i++) {
      printf("[menu] connessione fallita dopo %.1f s (%s): riprovo (%d)\n", now_s() - t0,
             gs_error ? gs_error : "?", i + 1);
      if (i == 0)
        mg_net_diag("connessione a Sunshine fallita", opt->host);
      sleep(2);
      t0 = now_s();
      ret = gs_init(&server, opt->host, 47989, keydir, 0, true);
    }
    if (ret == GS_OK && !server.paired)
      printf("[menu] serverinfo: non abbinato secondo HTTP (app in corso %d); HTTPS fallito dopo %.1f s: %s\n",
             server.currentGame, now_s() - t0, gs_https_error[0] ? gs_https_error : "?");
    /* Subito dopo uno stream Sunshine può non rispondere sulla porta HTTPS:
     * libgamestream ripiega allora su HTTP, che dice sempre "non abbinato".
     * Se l'abbinamento c'era, si riprova qualche volta prima di chiedere il PIN. */
    for (int i = 0; i < 5 && ret == GS_OK && !server.paired && was_paired && !mg_exiting(); i++) {
      printf("[menu] Sunshine risponde \"non abbinato\": riprovo (%d)\n", i + 1);
      sleep(2);
      t0 = now_s();
      ret = gs_init(&server, opt->host, 47989, keydir, 0, true);
      if (ret == GS_OK && !server.paired)
        printf("[menu]   ancora no: HTTPS fallito dopo %.1f s: %s\n", now_s() - t0, gs_https_error);
    }
    if (ret == GS_OK && server.paired)
      was_paired = true;
    if (ret != GS_OK) {
      snprintf(job_error, sizeof(job_error), "%s", gs_error ? gs_error : "nessuna risposta");
    } else if (server.paired) {
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
  if (job != JOB_NONE) return;
  job = j;
  job_done = 0;
  pthread_create(&job_thread, NULL, job_main, NULL);
}

static void rebuild_items(void);

static void finish_job(void) {
  if (job == JOB_NONE || !job_done) return;
  pthread_join(job_thread, NULL);
  int j = job;
  job = JOB_NONE;
  if (j == JOB_CONNECT) {
    if (job_result != GS_OK) {
      char l1[160];
      snprintf(l1, sizeof(l1), "Sunshine non risponde su %s", opt->host);
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
  case IT_EXIT: snprintf(label, ln, "Esci"); break;
  }
}

/* ---------- schermate ---------- */

static void draw_frame(const char *footer) {
  fill(0, 0, W, H, C_BG);
  text(X0, Y0, "MOONLIGHT", C_HEAD);
  text_right(W - X0, Y0, opt->host, C_DIM);
  fill(X0, Y0 + 12, W - 2 * X0, 1, C_LINE);
  fill(X0, H - Y0 - 12, W - 2 * X0, 1, C_LINE);
  if (status[0] && now_s() < status_until)
    text(X0, H - Y0 - 24, status, status_color);
  if (footer)
    text(X0, H - Y0 - 8, footer, C_DIM);
}

static void draw_main(void) {
  draw_frame("A avvia/ok  < > cambia  B esci");
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
  draw_frame("B esci");
  char l[96];
  snprintf(l, sizeof(l), "Connessione a %s...", opt->host);
  text_center(100, l, C_TEXT, 1);
  static const char spin[] = "|/-\\";
  char s[2] = { spin[(int)(now_s() * 6) % 4], 0 };
  text_center(120, s, C_HEAD, 2);
}

static void draw_pair(void) {
  draw_frame("B esci");
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
  draw_frame(msg_retry ? "A riprova  B esci" : "A ok");
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
  switch (state) {
  case ST_CONNECT:
  case ST_PAIR:
    if (k == MG_KEY_BACK) return false;
    break;
  case ST_MESSAGE:
    if (k == MG_KEY_OK) {
      if (msg_retry) { state = ST_CONNECT; start_job(JOB_CONNECT); }
      else state = server_ok ? ST_MAIN : ST_CONNECT;
    } else if (k == MG_KEY_BACK) {
      if (msg_retry || !server_ok) return false;
      state = ST_MAIN;
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
      return false;
    } else if (k == MG_KEY_OK) {
      if (t == IT_APP) {
        if (job != JOB_NONE) set_status("Attendi l'aggiornamento...", C_DIM);
        else launch(items[cursor].app);
      }
      else if (t == IT_EXIT) return false;
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

  mg_input_start(o->pads, mg_request_quit);
  mg_input_set_mode(MG_INPUT_MENU);

  state = ST_CONNECT;
  start_job(JOB_CONNECT);
  double last_present = 0;
  bool dirty = true;
  while (!mg_exiting()) {
    int k = mg_input_menu_key(dirty ? 0 : 100);
    if (k != MG_KEY_NONE) dirty = true;
    if (!handle_key(k)) break;
    if (job != JOB_NONE && job_done) { finish_job(); dirty = true; }
    double t = now_s();
    /* ridisegna se serve, e comunque ogni tanto (animazione, messaggi a tempo, Groovy) */
    if (dirty || t - last_present > (state == ST_CONNECT ? 0.15 : 1.0)) {
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
