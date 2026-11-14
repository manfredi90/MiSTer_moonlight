/*
 * Uscita sul core H264 del MiSTer (decoder H.264 in FPGA, owenb321/MiSTer_h264_decoder):
 * ogni decode unit di Sunshine diventa un record VIDEO_AU nel ring in DDR3, l'audio
 * Opus decodificato qui un record AUDIO, il menu un overlay nel framebuffer di Linux
 * che il core sovrappone al video. Interfaccia: third_party/h264ring (h264ring.h).
 *
 * La gestione della sessione, degli errori e del battito del core segue il client
 * Moonlight di riferimento del progetto (clients/moonlight/src/mister/mister.c):
 * - LOW_LATENCY | GRAPHICS su ogni access unit, senza PTS: il core mostra ogni
 *   immagine appena decodificata e suona l'audio appena arriva.
 * - Una sola mutex attorno a ogni chiamata alla libreria (non è thread-safe); un
 *   ring pieno si aspetta con le chiamate try_ e una pausa fuori dalla mutex.
 * - Errori di decodifica (contatori del core) -> LiRequestIdrFrame. Una sessione
 *   persa (Reset dall'OSD) si riapre; un rifiuto dello stream lo chiude.
 * - Core scaricato (un altro core caricato dall'OSD): il battito del blocco di
 *   stato si ferma; si esce dal programma.
 * - L'audio aspetta il primo IDR della sessione: la modalità dello stream (a bassa
 *   latenza) la decide il suo primo VIDEO_AU.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */
#define _GNU_SOURCE
#include "core.h"

#include "h264ring.h"
#include "h264ring_cavlc.h"

#include <opus_multistream.h>

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define AU_FLAGS        (H264RING_F_LOW_LATENCY | H264RING_F_GRAPHICS)
#define VIDEO_WAIT_MS   500     /* ring pieno così a lungo: il decoder è indietro, si scarta l'AU */
#define AUDIO_WAIT_MS   20      /* l'audio si scarta piuttosto che aspettare (il core copre il buco) */
#define LAUNCH_MS       15000
#define SESSION_MS      2000
#define FLUSH_MS        1000
#define ALIVE_MS        1500    /* niente battito per questo tempo: il core non c'è più */
#define IDR_RETRY_MS    1000
#define IDR_GRACE_MS    250     /* errori subito dopo un IDR: riguardano le immagini prima */
#define RESTART_MS      1000
#define W_EARLY         1
#define OVL_MAX_H       576

static h264ring *ring;
static pthread_mutex_t ring_mx = PTHREAD_MUTEX_INITIALIZER;
static void (*gone_cb)(void);
static void (*refused_cb)(const char *why);
static char refused_why[160];

static bool session_ok, streaming, video_started, need_idr, gone;
static uint32_t err_base, refused_base, beat;
static uint64_t beat_ms, idr_asked_ms, idr_written_ms, restart_ms;
static volatile bool restarting, osd_open;
static bool cavlc_refused;

static uint8_t *au_buf;
static size_t au_cap;

static OpusMSDecoder *opus;
static int16_t *pcm;
static int pcm_frames, pcm_channels;
static int16_t *pcm2;

static mg_core_counters n;

/* ---------- il ritmo delle immagini ----------
 * In bassa latenza il core prende, a riga ~500 di ogni quadro, la più vecchia immagine decodificata;
 * se ne aspetta già un'altra dietro, la più vecchia la scarta (riga 490). Se le immagini arrivano
 * vicino a quel momento, un ritardo di qualche ms (Wi-Fi, decodifica) le fa cadere ora prima ora
 * dopo: un quadro ripetuto e poi un'immagine scartata, a raffiche, ogni volta che il ritmo del PC
 * (59,95 per esempio) porta gli arrivi su quel punto.
 *
 * NOTA (0.1.5): il core chiude un'immagine solo quando arriva l'inizio della successiva (la chiusura
 * a fine record, "P3e", non c'è ancora): finché è così il passo non può funzionare (aspetterebbe la
 * presa di un'immagine che si completa solo scrivendo la successiva) ed è spento in modo predefinito.
 *
 * Il passo (pacing): al core si scrive un'immagine solo quando la precedente è già stata presa (o
 * scartata). Dopo un arrivo in ritardo (un quadro ripetuto) le immagini seguenti vengono scritte
 * subito dopo ogni presa, con un quadro intero per decodificarle: niente più raffiche, solo la
 * ripetizione che lo scarto di ritmo col PC rende inevitabile. Si scrive comunque se dietro c'è già
 * un'immagine più nuova (non si accumula ritardo) o dopo PACE_MAX_MS (un'immagine persa).
 *
 * Con -stats: ogni immagine saltata / ripetuta all'istante, e ogni 10 s un riepilogo. */
#define RITMO_PERIOD_S 10
#define PACE_MAX_MS    50
static bool pacing = false;
static struct {
  bool on;
  uint32_t pick_last, skip_last, starve_last;
  uint64_t t0_ms, t_last_ms;
  /* finestra corrente */
  unsigned frames, held, held_us_max, forced, lost;
  uint64_t held_us_sum;
  uint32_t rtp_first, rtp_last;
  int64_t jit_min, jit_max;    /* arrivo - tempo del PC (rtp), us */
  unsigned skips, starves, picks;
} rt;
static uint32_t pace_written, pace_base;   /* scritte dall'inizio dello stream; prese + scartate all'inizio */

void mg_core_set_ritmo(bool on) { rt.on = on; }
void mg_core_set_pacing(bool on) { pacing = on; }
static bool guard_on;
void mg_core_set_guard(bool on) { guard_on = on; }

static uint64_t now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}


/* overlay */
static struct h264ring_fb fb;
static bool fb_ok;
static unsigned ovl_fmt, ovl_bpp, ovl_pages, ovl_page;
static uint32_t ovl_page_bytes;
static int ovl_gen, ovl_w, ovl_h;   /* ovl_w: la larghezza del raster dell'ultimo disegno */
static int ovl_max_w;
static bool ovl_shown;

static uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

struct h264ring *mg_core_ring(void) { return ring; }
bool mg_core_osd_open(void) { return osd_open; }

static uint32_t err_sum(const struct h264ring_status *st) {
  return (uint32_t)(H264RING_ST(st, FE_ERR) + H264RING_ST(st, RC_ERR));
}

static const char *refusal_name(unsigned code) {
  static const char *const names[] = {
    "nessuno", "formato colore (non 4:2:0)", "profondita' (non 8 bit)", "scaling list",
    "interlacciato MBAFF", "CAVLC (non CABAC)", "FMO", "trasformata 8x8",
    "immagini ridondanti", "slice SP/SI", "data partitioning", "SVC/MVC",
    "dimensioni (oltre 128 macroblocchi)",
  };
  return code < sizeof(names) / sizeof(names[0]) ? names[code] : "sconosciuto";
}

static void core_gone_locked(void) {
  if (gone)
    return;
  gone = true;
  session_ok = false;
  printf("[core] il core H264 non risponde piu' (caricato un altro core?): esco\n");
  if (gone_cb)
    gone_cb();
}

static int session_start_locked(void) {
  restart_ms = now_ms();
  restarting = true;
  int rc = h264ring_start_session(ring, SESSION_MS);
  if (rc && n.sessions > 0 && h264ring_alive(ring, ALIVE_MS) != 0) {
    restarting = false;
    core_gone_locked();
    return rc;
  }
  restarting = false;
  if (rc) {
    printf("[core] sessione non aperta (%s)\n", strerror(-rc));
    session_ok = false;
    return rc;
  }
  struct h264ring_status st;
  if (h264ring_status(ring, &st) == 0) {
    err_base = err_sum(&st);
    refused_base = (uint32_t)H264RING_ST(&st, FE_REFUSED);
  }
  session_ok = true;
  video_started = false;
  need_idr = true;
  n.sessions++;
  if (n.sessions > 1)
    printf("[core] sessione riaperta (%lu)\n", n.sessions);
  return 0;
}

static void poll_status_locked(void) {
  struct h264ring_status st;
  if (!ring || gone || h264ring_status(ring, &st))
    return;
  uint32_t seq = (uint32_t)H264RING_ST(&st, SEQ_HEAD);
  if (seq != beat || !beat_ms) {
    beat = seq;
    beat_ms = now_ms();
  } else if (now_ms() - beat_ms >= ALIVE_MS) {
    core_gone_locked();
    return;
  }
  osd_open = (H264RING_ST(&st, LIVE) & H264RING_L_OSD_OPEN) != 0;
  if (H264RING_ST(&st, STATE) == H264RING_S_ERROR && session_ok) {
    printf("[core] il core ha fermato la sessione: %s a %u\n",
           h264ring_error_name((unsigned)H264RING_ST(&st, ERR)), (unsigned)H264RING_ST(&st, ERR_POS));
    session_ok = false;
  }
  if (!streaming)
    return;
  uint32_t refused = (uint32_t)H264RING_ST(&st, FE_REFUSED);
  if (refused != refused_base && session_ok) {
    printf("[core] il core rifiuta lo stream: %s\n", refusal_name((unsigned)H264RING_ST(&st, FE_REFUSED_CODE)));
    refused_base = refused;
    n.refusals++;
    cavlc_refused = true; /* niente più AU: un IDR non lo cura */
    snprintf(refused_why, sizeof(refused_why), "Il core rifiuta lo stream: %s",
             refusal_name((unsigned)H264RING_ST(&st, FE_REFUSED_CODE)));
    if (refused_cb)
      refused_cb(refused_why);
    return;
  }
  uint32_t e = err_sum(&st);
  if (e != err_base && idr_written_ms && now_ms() - idr_written_ms < IDR_GRACE_MS) {
    n.errors_seen += e - err_base;
    err_base = e;
  } else if (e != err_base) {
    n.errors_seen += e - err_base;
    err_base = e;
    if (!need_idr || (idr_asked_ms && now_ms() - idr_asked_ms > IDR_RETRY_MS)) {
      need_idr = true;
      idr_asked_ms = now_ms();
      n.idr_asked++;
      LiRequestIdrFrame();
    }
  }
}

static void ritmo_reset_window(void) {
  rt.frames = rt.held = rt.held_us_max = rt.forced = rt.lost = 0;
  rt.held_us_sum = 0;
  rt.jit_min = INT64_MAX;
  rt.jit_max = INT64_MIN;
  rt.skips = rt.starves = rt.picks = 0;
}

/* Ogni ~2 ms dal thread dell'input durante lo stream: gli scatti, all'istante */
void mg_core_tick(void) {
  if (!rt.on || !streaming || restarting)
    return;
  struct h264ring_status st;
  pthread_mutex_lock(&ring_mx);
  int ok = ring && !gone && h264ring_status(ring, &st) == 0;
  pthread_mutex_unlock(&ring_mx);
  if (!ok)
    return;
  uint32_t pick = (uint32_t)H264RING_ST(&st, DISP_PICK), skip = (uint32_t)H264RING_ST(&st, DISP_SKIP);
  uint32_t starve = (uint32_t)H264RING_ST(&st, DISP_STARVE);
  if (!rt.t0_ms) {
    rt.t0_ms = rt.t_last_ms = now_ms();
    rt.pick_last = pick; rt.skip_last = skip; rt.starve_last = starve;
    ritmo_reset_window();
  }
  double t = (now_ms() - rt.t0_ms) / 1000.0;
  rt.picks += pick - rt.pick_last;
  rt.pick_last = pick;
  if (skip != rt.skip_last) {
    printf("[ritmo] %7.2f s: %u immagini scartate (due pronte nello stesso quadro)\n", t, skip - rt.skip_last);
    rt.skips += skip - rt.skip_last;
    rt.skip_last = skip;
  }
  if ((uint16_t)(starve - rt.starve_last)) {
    printf("[ritmo] %7.2f s: %u quadri senza immagine nuova (ripetuta)\n", t, (uint16_t)(starve - rt.starve_last));
    rt.starves += (uint16_t)(starve - rt.starve_last);
    rt.starve_last = starve;
  }
}

static void guard_report(void);
static void ritmo_au(PDECODE_UNIT du, uint64_t held_us, bool forced) {
  if (!rt.on || !rt.t0_ms)
    return;
  int64_t jit = (int64_t)du->receiveTimeUs - (int64_t)((uint64_t)du->rtpTimestamp * 1000000ull / 90000ull);
  if (!rt.frames)
    rt.rtp_first = du->rtpTimestamp;
  rt.frames++;
  rt.rtp_last = du->rtpTimestamp;
  if (jit < rt.jit_min) rt.jit_min = jit;
  if (jit > rt.jit_max) rt.jit_max = jit;
  if (held_us) {
    rt.held++;
    rt.held_us_sum += held_us;
    if (held_us > rt.held_us_max) rt.held_us_max = (unsigned)held_us;
  }
  if (forced) rt.forced++;
  if (now_ms() - rt.t_last_ms >= RITMO_PERIOD_S * 1000u && rt.frames > 10) {
    double t = (now_ms() - rt.t0_ms) / 1000.0;
    rt.t_last_ms = now_ms();
    double drtp = (double)(uint32_t)(rt.rtp_last - rt.rtp_first);
    double fps = drtp > 0 ? (rt.frames - 1) * 90000.0 / drtp : 0;
    printf("[ritmo] %7.1f s: %u immagini, il PC a %.3f fps, arrivo +-%.1f ms; passo %s: %u trattenute "
           "(media %.1f ms, max %.1f), %u scritte con una piu' nuova dietro, %u perse; "
           "core: %u prese, %u scartate, %u ripetute\n",
           t, rt.frames, fps, (rt.jit_max - rt.jit_min) / 2000.0, guard_on ? "guardia" : pacing ? "attivo" : "spento", rt.held,
           rt.held ? rt.held_us_sum / 1000.0 / rt.held : 0.0, rt.held_us_max / 1000.0, rt.forced, rt.lost,
           rt.picks, rt.skips, rt.starves);
    guard_report();
    ritmo_reset_window();
  }
}

/* Aspetta che l'immagine precedente sia stata presa o scartata dal core (il passo) */
static void pace_wait(uint64_t *held_us, bool *forced) {
  *held_us = 0;
  *forced = false;
  if (!pacing)
    return;
  uint64_t t0 = now_us();
  for (;;) {
    struct h264ring_status st;
    pthread_mutex_lock(&ring_mx);
    int ok = ring && session_ok && streaming && h264ring_status(ring, &st) == 0;
    pthread_mutex_unlock(&ring_mx);
    if (!ok)
      return;
    uint32_t done = (uint32_t)H264RING_ST(&st, DISP_PICK) + (uint32_t)H264RING_ST(&st, DISP_SKIP) - pace_base;
    int32_t pending = (int32_t)(pace_written - done);
    if (pending <= 0) {
      if (pending < 0)
        pace_base += (uint32_t)pending; /* contate più prese che scritte (riavvio): si riallinea */
      break;
    }
    uint64_t dt = now_us() - t0;
    if (LiGetPendingVideoFrames() > 0) {
      *forced = true; /* ne è arrivata un'altra: niente ritardo accumulato, il core scarta la vecchia */
      break;
    }
    if (dt >= PACE_MAX_MS * 1000u) {
      /* un'immagine mai presa (persa dal decoder): non la si aspetta più */
      pace_base -= (uint32_t)pending;
      if (rt.on) rt.lost++;
      break;
    }
    usleep(300);
  }
  *held_us = now_us() - t0;
  if (*held_us < 500)
    *held_us = 0;
}

/* ---------- la guardia di fase (-guard; core con la P3e) ----------
 * Col PC a 60,000 Hz esatti gli arrivi restano a lungo nella stessa posizione rispetto alla presa del
 * core: lontani da essa, tutto liscio; vicini, ogni ritardo di rete di qualche ms fa cadere
 * un'immagine ora in un quadro ora nel successivo (una ripetuta e una scartata, di continuo).
 *
 * La guardia misura la griglia del core sull'orologio dell'ARM: il blocco di stato si riscrive a
 * ogni quadro (SEQ_HEAD +1, all'inizio del quadro), la valutazione della presa cade a 428.142 / 450.000
 * del quadro (425.947 col 240p; h264/disp_ctl.sv). Un thread legge SEQ_HEAD ogni ~0,4 ms e un PLL
 * ne ricava la griglia. Su questa: la fase d'arrivo media phi delle immagini, la loro dispersione s, e
 * D, il tempo che il core impiega a decodificare un'immagine (imparato: un'immagine scritta in tempo
 * per una presa che invece la manca alza D).
 *
 * Finché la nuvola degli arrivi (phi +- s) resta lontana dalla scadenza della decodifica (P - D, con
 * l'incertezza di D) la guardia non fa nulla. Se ci si avvicina, mette un taglio c appena prima della
 * nuvola: le immagini che arrivano dopo c vengono trattenute fino alla prossima presa e scritte
 * subito dopo, così vanno tutte, con certezza, alla presa successiva (un quadro di latenza in più,
 * solo finché la fase è quella); quelle prima di c vanno alla prossima. La presa a cui va
 * un'immagine dipende allora da dove cade rispetto a c, lontano dalla nuvola, e non dal caso. Con
 * isteresi: il taglio si sposta se la nuvola gli si avvicina, e la guardia si spegne se la nuvola si
 * allontana dalla scadenza con largo margine per qualche secondo; con arrivi sparsi su più di
 * mezzo quadro nessun taglio aiuta e resta spenta. */
#define G_POLL_US     400
#define G_MARGIN_US   1000.0
#define G_D0_US       10500.0     /* D iniziale: 1.200 macroblocchi a ~110.000 al secondo */
#define G_D_MIN_US    5000.0
#define G_D_MAX_US    15000.0
static struct {
  pthread_t th;
  volatile bool run;
  bool th_ok;
  pthread_mutex_t mx;
  /* la griglia (protetta da mx) */
  double P, eoff;                  /* periodo e posizione della valutazione nel quadro, us */
  double t_ref;                    /* l'inizio di un quadro, us (CLOCK_MONOTONIC) */
  unsigned samples, outliers;
  /* il blocco di stato all'ultimo quadro */
  uint32_t pick, starve;
  bool have_snap;
  /* gli arrivi */
  double ex, ey;                   /* media circolare (EMA) */
  double mad;                      /* scarto medio assoluto dalla fase media, us */
  unsigned arrivals;
  double D;                        /* tempo di decodifica stimato, us */
  int mode;                        /* 0 taglio alla scadenza, 1 taglio fisso */
  double cut, fixed_cut;
  unsigned good_run;               /* immagini col taglio preferito libero (per tornarci) */
  /* immagini scritte in attesa dell'esito: l'istante della scrittura e la presa attesa */
  struct { double wr, ev; bool idr; } pend[4];
  unsigned npend;
  /* contatori per -stats */
  unsigned held, misses, switches, hits;
  double held_us_sum;
} g = { .mx = PTHREAD_MUTEX_INITIALIZER };

static double g_wrap(double x, double P) {
  x = fmod(x, P);
  return x < 0 ? x + P : x;
}
static double g_cdist(double a, double b, double P) {   /* distanza circolare */
  double d = fabs(g_wrap(a - b, P));
  return d > P / 2 ? P - d : d;
}

/* il thread della griglia */
static void *guard_thread(void *arg) {
  (void)arg;
  volatile uint64_t *ctrl = NULL;
  pthread_mutex_lock(&ring_mx);
  if (ring)
    ctrl = ((struct h264ring_backend *)ring)->ctrl;   /* be è il primo campo di struct h264ring */
  pthread_mutex_unlock(&ring_mx);
  if (!ctrl)
    return NULL;
  uint32_t last = (uint32_t)ctrl[H264RING_SEQ_HEAD_W];
  double t_prev = (double)now_us();
  while (g.run) {
    usleep(G_POLL_US);
    double t = (double)now_us();
    uint32_t seq = (uint32_t)ctrl[H264RING_SEQ_HEAD_W];
    if (seq != last) {
      double s = 0.5 * (t_prev + t);   /* il cambio è caduto tra le due letture */
      struct h264ring_status st;
      bool snap = false;
      pthread_mutex_lock(&ring_mx);
      if (ring && !gone)
        snap = h264ring_status(ring, &st) == 0;
      pthread_mutex_unlock(&ring_mx);
      pthread_mutex_lock(&g.mx);
      if (!g.samples) {
        g.t_ref = s;
      } else {
        double k = floor((s - g.t_ref) / g.P + 0.5);
        double err = s - (g.t_ref + k * g.P);
        if (fabs(err) > 2500 && g.samples > 60) {
          if (++g.outliers > 30) g.samples = 0, g.outliers = 0;   /* persa: da capo */
        } else {
          g.outliers = 0;
          g.t_ref += k * g.P + err * (g.samples < 60 ? 0.125 : 0.03);
        }
      }
      if (g.samples < 100000) g.samples++;
      if (snap) {
        uint32_t pick = (uint32_t)H264RING_ST(&st, DISP_PICK);
        uint32_t starve = (uint32_t)H264RING_ST(&st, DISP_STARVE) & 0xffffu;
        if (g.have_snap && g.samples > 60) {
          /* la valutazione appena passata: il quadro iniziato a s, meno un periodo, più eoff */
          double ev = s - g.P + g.eoff;
          bool starved = ((starve - g.starve) & 0xffffu) != 0;
          bool picked = pick != g.pick;
          unsigned j = 0;
          for (unsigned i = 0; i < g.npend; i++) {
            if (fabs(g.pend[i].ev - ev) < g.P / 2) {
              double slack = g.pend[i].ev - g.pend[i].wr;   /* il tempo che aveva per decodificarsi */
              if (starved && !picked) {
                g.misses++;
                if (!g.pend[i].idr && slack + 500 > g.D) g.D = fmin(G_D_MAX_US, slack + 500);
              } else if (picked) {
                g.hits++;
                if (!g.pend[i].idr && slack < g.D + 2000) g.D = fmax(G_D_MIN_US, g.D - 5);
              }
            } else if (g.pend[i].ev > ev) {
              g.pend[j++] = g.pend[i];                        /* non ancora */
            }
          }
          g.npend = j;
        }
        g.pick = pick;
        g.starve = starve;
        g.have_snap = true;
      }
      pthread_mutex_unlock(&g.mx);
      last = seq;
    }
    t_prev = t;
  }
  return NULL;
}

static void guard_start(void) {
  if (!guard_on || g.th_ok)
    return;
  mg_core_info ci;
  mg_core_get_info(&ci);
  pthread_mutex_lock(&g.mx);
  unsigned hz100 = ci.refresh_x100 ? ci.refresh_x100 : 6000;
  g.P = 1e8 / hz100;
  g.eoff = g.P * (ci.p240 ? 425947.0 : 428142.0) / 450000.0;
  g.samples = g.outliers = 0;
  g.have_snap = false;
  g.ex = g.ey = 0;
  g.mad = 0;
  g.arrivals = 0;
  if (g.D < G_D_MIN_US) g.D = G_D0_US;   /* resta tra uno stream e l'altro */
  g.mode = 0;
  g.good_run = 0;
  g.npend = 0;
  g.held = g.misses = g.switches = g.hits = 0;
  g.held_us_sum = 0;
  pthread_mutex_unlock(&g.mx);
  g.run = true;
  g.th_ok = pthread_create(&g.th, NULL, guard_thread, NULL) == 0;
}

static void guard_stop(void) {
  if (!g.th_ok)
    return;
  g.run = false;
  pthread_join(g.th, NULL);
  g.th_ok = false;
}

/* Prima di scrivere un'immagine: trattenerla fino alla prossima presa se cade dopo il taglio */
static void guard_wait(bool idr, uint64_t *held_us) {
  *held_us = 0;
  if (!g.th_ok)
    return;
  double w = (double)now_us();
  pthread_mutex_lock(&g.mx);
  if (g.samples < 90) {
    pthread_mutex_unlock(&g.mx);
    return;
  }
  double P = g.P;
  double e0 = g.t_ref + g.eoff;              /* una valutazione */
  double a = g_wrap(w - e0, P);              /* da quanto è passata l'ultima valutazione */
  double e_next = w - a + P;
  /* la fase media e la dispersione degli arrivi */
  double ang = 2 * M_PI * a / P;
  double al = g.arrivals < 64 ? 1.0 / (g.arrivals + 1) : 1.0 / 64;
  g.ex += (cos(ang) - g.ex) * al;
  g.ey += (sin(ang) - g.ey) * al;
  double phi = g_wrap(atan2(g.ey, g.ex) * P / (2 * M_PI), P);
  g.mad += (g_cdist(a, phi, P) - g.mad) * al;
  g.arrivals++;
  /* la zona di pericolo: attorno alla scadenza (P - D: dopo, l'immagine non è pronta per la prossima
   * presa; D varia da un'immagine all'altra, la zona è larga di conseguenza) */
  double d = P - g.D;
  double zc = d + 500;
  double h = 3 * g.mad + G_MARGIN_US;        /* la nuvola degli arrivi, con margine */
  double zw = h + 1500;
  if (g.arrivals > 60) {
    if (2 * h > 0.8 * P) {
      if (g.mode) g.switches++;
      g.mode = 0;                            /* arrivi troppo sparsi: nessun taglio aiuta */
    } else if (g.mode == 0 && g_cdist(phi, zc, P) < 0.8 * zw) {
      g.mode = 1; g.fixed_cut = g_wrap(phi - h, P); g.switches++; g.good_run = 0;
    } else if (g.mode == 1) {
      if (g_cdist(phi, g.fixed_cut, P) < 0.6 * h) {
        if (g_cdist(phi, zc, P) > zw) g.mode = 0;
        else g.fixed_cut = g_wrap(phi - h, P);
        g.switches++;
        g.good_run = 0;
      } else if (g_cdist(phi, zc, P) > 1.3 * zw) {
        if (++g.good_run > 180) { g.mode = 0; g.switches++; g.good_run = 0; }
      } else {
        g.good_run = 0;
      }
    }
  }
  /* modo 0 (arrivi lontani dalla scadenza): niente da fare. Modo 1: taglio appena prima della nuvola,
   * che va tutta alla presa successiva */
  double c = g.mode == 0 ? d : g.fixed_cut;
  g.cut = c;
  double hold_until = 0, target = a < d ? e_next : e_next + P;
  if (g.mode == 1) {
    if (c <= d) {
      if (a < c) target = e_next;
      else { target = e_next + P; hold_until = e_next + 200; }
    } else {
      if (a < c) { target = e_next + P; hold_until = e_next + 200; }
      else { target = e_next + 2 * P; hold_until = e_next + P + 200; }
    }
  }
  pthread_mutex_unlock(&g.mx);
  if (hold_until > w) {
    for (;;) {
      double t = (double)now_us();
      if (t >= hold_until || !g.run || !streaming)
        break;
      double left = hold_until - t;
      usleep(left > 2000 ? 1000 : (useconds_t)(left > 50 ? left : 50));
    }
    *held_us = (uint64_t)((double)now_us() - w);
  }
  pthread_mutex_lock(&g.mx);
  if (g.npend == 4) {
    memmove(&g.pend[0], &g.pend[1], 3 * sizeof(g.pend[0]));
    g.npend = 3;
  }
  g.pend[g.npend].wr = (double)now_us();
  g.pend[g.npend].ev = target;
  g.pend[g.npend].idr = idr;
  g.npend++;
  if (*held_us) { g.held++; g.held_us_sum += (double)*held_us; }
  pthread_mutex_unlock(&g.mx);
}

static void guard_report(void) {
  if (!g.th_ok)
    return;
  pthread_mutex_lock(&g.mx);
  double P = g.P;
  double phi = g_wrap(atan2(g.ey, g.ex) * P / (2 * M_PI), P);
  printf("[guardia] arrivi a %.1f ms dalla presa (dispersione %.1f ms), decodifica %.1f ms, taglio a %.1f ms (%s); "
         "%u trattenute (media %.1f ms), %u mancate, %u cambi di taglio\n",
         phi / 1000, g.mad / 1000, g.D / 1000, g.cut / 1000, g.mode ? "attivo" : "spento (arrivi lontani)",
         g.held, g.held ? g.held_us_sum / 1000 / g.held : 0.0, g.misses, g.switches);
  g.held = g.misses = g.switches = g.hits = 0;
  g.held_us_sum = 0;
  pthread_mutex_unlock(&g.mx);
}

void mg_core_poll(void) {
  if (restarting)
    return;
  pthread_mutex_lock(&ring_mx);
  poll_status_locked();
  /* fuori dallo stream (menu): una sessione persa si riapre subito, se no
   * l'overlay sparisce */
  if (ring && !gone && !session_ok && !streaming && now_ms() - restart_ms >= RESTART_MS)
    session_start_locked();
  pthread_mutex_unlock(&ring_mx);
}

void mg_core_get_info(mg_core_info *ci) {
  struct h264ring_status st;
  memset(ci, 0, sizeof(*ci));
  pthread_mutex_lock(&ring_mx);
  int got = ring && h264ring_status(ring, &st) == 0;
  pthread_mutex_unlock(&ring_mx);
  if (!got)
    memset(&st, 0, sizeof(st));
  unsigned iface = (unsigned)(st.w[1] & 0xFFFFu);
  if (iface >= 0x103u) {
    /* il core di core/ (interfaccia 1.3+): Output Mode e 240p Playback nella parola 87.
     * Il 240p/288p ha righe di lunghezza costante: 27 MHz / (1720 x 262) = 59,914 Hz,
     * 27 MHz / (1728 x 312,5) = 49,993 Hz */
    uint64_t w87 = st.w[87];
    unsigned std = (unsigned)(w87 >> 11) & 3u;
    bool il = (w87 >> 13) & 1u, p240 = (w87 >> 4) & 1u;
    ci->p240 = p240 && il;
    if (ci->p240)
      ci->refresh_x100 = std == 2 ? 4999 : std == 1 ? 6000 : 5991;   /* 1: il core di core/ (60,000 Hz) */
    else
      ci->refresh_x100 = std == 2 ? 5000 : std == 1 ? 6000 : 5994;
    ci->interlaced = il && !ci->p240;
  } else {
    unsigned refresh = (unsigned)H264RING_ST(&st, OSD_REFRESH);
    unsigned std = (unsigned)H264RING_ST(&st, RASTER_STD);
    /* Refresh Rate: 0 Auto (segue il ritmo dello stream: chiedendo 59.94 a Sunshine
     * resta 59.94), 1 59.94, 2 60, 3 50 */
    if (refresh == 3 || (refresh == 0 && std == 2))
      ci->refresh_x100 = 5000;
    else if (refresh == 2)
      ci->refresh_x100 = 6000;
    else
      ci->refresh_x100 = 5994;
    ci->interlaced = H264RING_ST(&st, RASTER_IL) != 0;
  }
  ci->raster_h = ci->refresh_x100 == 5000 || ci->refresh_x100 == 4999 ? 576 : 480;
  struct h264ring_raster rs;
  h264ring_raster(&st, &rs);
  ci->raster_w = rs.mode == H264RING_RASTER_FITTED && rs.w ? (int)rs.w : 720;
  struct h264ring_caps caps;
  h264ring_caps(&st, &caps);
  ci->max_mbs = caps.max_mbs;
  ci->mb_rate = caps.mb_rate;
}

/* ---------- overlay ---------- */

static bool fb_mapped; /* da h264ring_fb_map (non un framebuffer dei test) */

static void overlay_layout(void) {
  ovl_max_w = 720;       /* il raster più largo: 720 (core originale); 640 quello di core/ */
  ovl_w = 720;
  ovl_h = OVL_MAX_H;
  static const struct { unsigned fmt, bpp, pages; } tries[] = {
    { H264RING_OVL_FMT_8888, 4, 2 }, { H264RING_OVL_FMT_1555, 2, 2 },
    { H264RING_OVL_FMT_8888, 4, 1 }, { H264RING_OVL_FMT_1555, 2, 1 },
  };
  for (size_t i = 0; i < sizeof(tries) / sizeof(tries[0]); i++) {
    uint32_t pb = (uint32_t)ovl_max_w * tries[i].bpp * (uint32_t)ovl_h;
    if (pb * tries[i].pages <= fb.len) {
      ovl_fmt = tries[i].fmt;
      ovl_bpp = tries[i].bpp;
      ovl_pages = tries[i].pages;
      ovl_page_bytes = pb;
      fb_ok = true;
      break;
    }
  }
  if (!fb_ok) {
    printf("[core] framebuffer troppo piccolo (%u byte): niente menu a schermo\n", (unsigned)fb.len);
    if (fb_mapped)
      h264ring_fb_unmap(&fb);
    fb_mapped = false;
    return;
  }
  memset(fb.virt, 0, (size_t)ovl_page_bytes * ovl_pages);
  printf("[core] overlay fino a %dx%d, %u byte per pixel, %u pagine (framebuffer %u byte)\n", ovl_max_w, ovl_h, ovl_bpp,
         ovl_pages, (unsigned)fb.len);
}

static void overlay_setup(void) {
  int rc = h264ring_fb_map(&fb);
  if (rc) {
    printf("[core] framebuffer non disponibile (%s): niente menu a schermo\n", strerror(-rc));
    return;
  }
  fb_mapped = true;
  overlay_layout();
}

void mg_core_use_fb(void *virt, uint32_t phys, uint32_t len) {
  memset(&fb, 0, sizeof(fb));
  fb.virt = virt;
  fb.phys = phys;
  fb.len = len;
  fb.fd = -1;
  fb_mapped = false;
  overlay_layout();
}

/* un pixel nel formato dell'overlay (8888: byte B, G, R, A = 0xAARRGGBB; 1555: A R G B) */
static uint32_t ovl_px(uint32_t c) {
  if (ovl_bpp == 4)
    return c | 0xFF000000u;
  return 0x8000 | (((c >> 19) & 0x1F) << 10) | (((c >> 11) & 0x1F) << 5) | ((c >> 3) & 0x1F);
}

/* una riga del canvas allargata a tutto il raster (320 -> 720: x2,25, interpolata come fa il
 * core col video: sul CRT non si vedono colonne larghe 2 e 3 pixel alternate) */
static void ovl_line(uint8_t *dst, const uint32_t *src, int w) {
  if (ovl_w == 2 * w) {          /* raster di 640 (core/): x2 esatto, niente interpolazione */
    for (int x = 0; x < w; x++) {
      uint32_t v = ovl_px(src[x]);
      if (ovl_bpp == 4) {
        ((uint32_t *)dst)[2 * x] = v;
        ((uint32_t *)dst)[2 * x + 1] = v;
      } else {
        ((uint16_t *)dst)[2 * x] = (uint16_t)v;
        ((uint16_t *)dst)[2 * x + 1] = (uint16_t)v;
      }
    }
    return;
  }
  for (int x = 0; x < ovl_w; x++) {
    int pos = (int)(((2 * x + 1) * w * 256LL) / (2 * ovl_w)) - 128; /* centro del pixel, 1/256 */
    if (pos < 0) pos = 0;
    int i = pos >> 8, f = pos & 255;
    if (i >= w - 1) { i = w - 1; f = 0; }
    uint32_t a = src[i], b = f ? src[i + 1] : a, c = 0;
    for (int sh = 0; sh < 24; sh += 8) {
      uint32_t ca = (a >> sh) & 0xFF, cb = (b >> sh) & 0xFF;
      c |= ((ca * (256 - f) + cb * f + 128) >> 8) << sh;
    }
    if (ovl_bpp == 4)
      ((uint32_t *)dst)[x] = ovl_px(c);
    else
      ((uint16_t *)dst)[x] = (uint16_t)ovl_px(c);
  }
}

static void ovl_fill(uint8_t *dst, uint32_t c) {
  for (int x = 0; x < ovl_w; x++) {
    if (ovl_bpp == 4)
      ((uint32_t *)dst)[x] = ovl_px(c);
    else
      ((uint16_t *)dst)[x] = (uint16_t)ovl_px(c);
  }
}

/* Il canvas (w x h) su tutto il raster: largo quanto il raster (720 pixel a 13,5 MHz, la
 * larghezza piena del CRT), righe doppie, al centro in altezza a 576 righe (sopra e sotto
 * il colore del suo angolo): non si vede niente del core sotto */
int mg_core_overlay_present(const uint32_t *canvas, int w, int h) {
  if (!fb_ok || !ring || w < 2 || w > ovl_max_w || h * 2 > OVL_MAX_H)
    return -1;
  mg_core_info ci;
  mg_core_get_info(&ci);
  /* largo quanto il raster del core (parola 80): 720 sul core originale, 640 su quello di core/ */
  ovl_w = ci.raster_w >= 2 * w && ci.raster_w <= ovl_max_w ? ci.raster_w : ovl_max_w;
  int oh = ci.raster_h > 0 && ci.raster_h <= OVL_MAX_H ? ci.raster_h : 480;
  if (h * 2 > oh)
    oh = h * 2;
  /* la pagina da disegnare non deve essere quella che il core sta leggendo */
  unsigned page = ovl_pages > 1 ? ovl_page ^ 1 : 0;
  if (ovl_pages > 1 && ovl_gen > 0) {
    pthread_mutex_lock(&ring_mx);
    h264ring_overlay_wait(ring, ovl_gen, 50);
    pthread_mutex_unlock(&ring_mx);
  }
  uint8_t *base = (uint8_t *)fb.virt + (size_t)page * ovl_page_bytes;
  size_t stride = (size_t)ovl_w * ovl_bpp;
  int y0 = (oh - 2 * h) / 2;
  uint32_t border = canvas[0];
  static uint8_t line[2048 * 4];
  for (int y = 0; y < oh; y++) {
    int sy = (y - y0) >> 1;
    bool in = y >= y0 && sy < h;
    /* due righe uguali di fila: la seconda si copia dalla prima */
    if (in && ((y - y0) & 1)) {
      memcpy(base + (size_t)y * stride, line, stride);
      continue;
    }
    if (in)
      ovl_line(line, canvas + (size_t)sy * w, w);
    else
      ovl_fill(line, border);
    memcpy(base + (size_t)y * stride, line, stride);
  }
  int ovl_h_now = oh;
  __sync_synchronize();
  struct h264ring_overlay o;
  memset(&o, 0, sizeof(o));
  o.base = fb.phys + page * ovl_page_bytes;
  o.width = (uint16_t)ovl_w;
  o.height = (uint16_t)ovl_h_now;
  o.stride = (uint16_t)stride;
  o.format = (uint8_t)ovl_fmt;
  o.band_first = 0;
  o.band_last = (uint16_t)(ovl_h_now - 1);
  pthread_mutex_lock(&ring_mx);
  int gen = h264ring_overlay_show(ring, &o);
  pthread_mutex_unlock(&ring_mx);
  if (gen < 0) {
    printf("[core] overlay rifiutato (%s)\n", strerror(-gen));
    return gen;
  }
  if (!ovl_shown)
    printf("[core] menu a schermo visibile\n");
  ovl_gen = gen;
  ovl_page = page;
  ovl_shown = true;
  return 0;
}

void mg_core_overlay_hide(void) {
  if (!ring || !ovl_shown)
    return;
  pthread_mutex_lock(&ring_mx);
  int gen = h264ring_overlay_hide(ring);
  pthread_mutex_unlock(&ring_mx);
  if (gen > 0)
    ovl_gen = gen;
  ovl_shown = false;
}

/* ---------- apertura ---------- */

int mg_core_attach(h264ring *r, void (*on_gone)(void), void (*on_refused)(const char *why)) {
  gone_cb = on_gone;
  refused_cb = on_refused;
  pthread_mutex_lock(&ring_mx);
  ring = r;
  gone = false;
  int rc = session_start_locked();
  pthread_mutex_unlock(&ring_mx);
  return rc;
}

static const char *core_rbf; /* NULL: _Other/H264 */
void mg_core_set_rbf(const char *rbf) { core_rbf = (rbf && *rbf) ? rbf : NULL; }

int mg_core_open(void (*on_gone)(void), void (*on_refused)(const char *why)) {
  printf("[core] carico il core H264%s%s...\n", core_rbf ? " da " : "", core_rbf ? core_rbf : "");
  int rc = h264ring_launch_core(core_rbf, LAUNCH_MS);
  if (rc) {
    printf("[core] il core H264 non parte (%s): serve %s_*.rbf sulla SD\n", strerror(-rc),
           core_rbf ? core_rbf : "_Other/H264");
    return rc;
  }
  h264ring *r;
  rc = h264ring_open_devmem(&r, 0);
  if (rc) {
    printf("[core] collegamento al core fallito (%s)%s\n", strerror(-rc),
           rc == -EBUSY ? ": un altro programma lo sta usando" : "");
    return rc;
  }
  rc = mg_core_attach(r, on_gone, on_refused);
  if (rc)
    return rc;
  overlay_setup();
  mg_core_info ci;
  mg_core_get_info(&ci);
  struct h264ring_status st;
  pthread_mutex_lock(&ring_mx);
  unsigned iface = h264ring_status(ring, &st) == 0 ? (unsigned)(st.w[1] & 0xFFFFu) : 0;
  pthread_mutex_unlock(&ring_mx);
  printf("[core] collegato (interfaccia %u.%u): quadri %.2f Hz, %s, fino a %u macroblocchi (%u al secondo)\n",
         iface >> 8, iface & 0xFF, ci.refresh_x100 / 100.0,
         ci.p240 ? "240p durante lo stream" : ci.interlaced ? "interlacciato" : "progressivo", ci.max_mbs, ci.mb_rate);
  return 0;
}

void mg_core_close(void) {
  pthread_mutex_lock(&ring_mx);
  if (ring) {
    if (ovl_shown)
      h264ring_overlay_hide(ring);
    h264ring_close(ring);
  }
  ring = NULL;
  session_ok = false;
  pthread_mutex_unlock(&ring_mx);
  if (fb_ok && fb_mapped)
    h264ring_fb_unmap(&fb);
  fb_ok = fb_mapped = false;
  ovl_shown = false;
}

/* ---------- stream ---------- */

void mg_core_stream_begin(void) {
  mg_core_overlay_hide();
  pthread_mutex_lock(&ring_mx);
  if (ring && !gone) {
    if (!session_ok)
      session_start_locked();
    else if (h264ring_flush(ring, FLUSH_MS) != 0)
      session_start_locked();
    struct h264ring_status st;
    if (h264ring_status(ring, &st) == 0) {
      err_base = err_sum(&st);
      refused_base = (uint32_t)H264RING_ST(&st, FE_REFUSED);
    }
  }
  video_started = false;
  need_idr = true;
  idr_asked_ms = idr_written_ms = 0;
  cavlc_refused = false;
  rt.t0_ms = 0;
  {
    struct h264ring_status st;
    pace_written = 0;
    pace_base = 0;
    if (ring && h264ring_status(ring, &st) == 0)
      pace_base = (uint32_t)H264RING_ST(&st, DISP_PICK) + (uint32_t)H264RING_ST(&st, DISP_SKIP);
  }
  streaming = true;
  pthread_mutex_unlock(&ring_mx);
  guard_start();
}

void mg_core_stream_end(void) {
  guard_stop();
  pthread_mutex_lock(&ring_mx);
  streaming = false;
  if (ring && !gone && session_ok && h264ring_flush(ring, FLUSH_MS) != 0)
    session_ok = false;
  pthread_mutex_unlock(&ring_mx);
  mg_core_poll(); /* riapre la sessione se il flush l'ha persa */
}

static int write_record(bool audio, const void *p, uint32_t len, bool idr, unsigned wait_ms) {
  uint64_t t0 = now_ms();
  for (;;) {
    int rc;
    if (audio && restarting)
      return -ENOTCONN;
    pthread_mutex_lock(&ring_mx);
    if (!ring || !session_ok || !streaming) {
      rc = -ENOTCONN;
    } else if (audio && !video_started) {
      rc = W_EARLY;
    } else {
      rc = audio ? h264ring_try_write_audio(ring, p, len, -1)
                 : h264ring_try_write_au(ring, p, len, -1, AU_FLAGS);
      if (rc == 0 && idr) {
        video_started = true;
        idr_written_ms = now_ms();
      }
      if (rc == -ENOTCONN || rc == -EPROTO)
        session_ok = false;
    }
    pthread_mutex_unlock(&ring_mx);
    if (rc != -EAGAIN)
      return rc;
    if (now_ms() - t0 >= wait_ms)
      return -ETIMEDOUT;
    usleep(1000);
  }
}

static int video_setup(int videoFormat, int width, int height, int redrawRate, void *context, int drFlags) {
  if (!(videoFormat & VIDEO_FORMAT_MASK_H264)) {
    printf("[core] il core decodifica solo H.264\n");
    return -1;
  }
  if (!ring) {
    printf("[core] non collegato al core\n");
    return -1;
  }
  mg_core_info ci;
  mg_core_get_info(&ci);
  long mbs = (long)((width + 15) / 16) * ((height + 15) / 16);
  if (mbs > (long)ci.max_mbs) {
    printf("[core] %dx%d sono %ld macroblocchi: il core ne decodifica al massimo %u\n", width, height, mbs, ci.max_mbs);
    return -1;
  }
  if (mbs * redrawRate > (long)ci.mb_rate * 101 / 100)
    printf("[core] attenzione: %dx%d a %d fps supera i %u macroblocchi al secondo del core\n", width, height,
           redrawRate, ci.mb_rate);
  printf("[core] video %dx%d a %d fps\n", width, height, redrawRate);
  return 0;
}

static void video_cleanup(void) {
  free(au_buf);
  au_buf = NULL;
  au_cap = 0;
  pthread_mutex_lock(&ring_mx);
  if (ring) {
    struct h264ring_cavlc_stats cs;
    h264ring_rewrite_stats(ring, &cs);
    if (cs.units_rewritten)
      printf("[core] CAVLC: %llu access unit riscritte in CABAC sull'ARM (disattiva CAVLC in Sunshine)\n",
             (unsigned long long)cs.units_rewritten);
  }
  pthread_mutex_unlock(&ring_mx);
}

static int video_submit(PDECODE_UNIT du) {
  bool idr = du->frameType == FRAME_TYPE_IDR;

  pthread_mutex_lock(&ring_mx);
  if (ring && !session_ok && !gone && now_ms() - restart_ms >= RESTART_MS)
    session_start_locked();
  bool wait_idr = need_idr;
  bool ok = session_ok && !cavlc_refused;
  if (idr) {
    need_idr = false;
    idr_asked_ms = 0;
  }
  pthread_mutex_unlock(&ring_mx);

  if (!ok || (wait_idr && !idr)) {
    pthread_mutex_lock(&ring_mx);
    poll_status_locked();
    pthread_mutex_unlock(&ring_mx);
    n.au_dropped++;
    return cavlc_refused ? DR_OK : DR_NEED_IDR;
  }

  if ((size_t)du->fullLength > au_cap) {
    size_t cap = (size_t)du->fullLength * 2;
    uint8_t *b = realloc(au_buf, cap);
    if (!b) {
      n.au_dropped++;
      return DR_NEED_IDR;
    }
    au_buf = b;
    au_cap = cap;
  }
  size_t len = 0;
  for (PLENTRY e = du->bufferList; e != NULL; e = e->next) {
    memcpy(au_buf + len, e->data, (size_t)e->length);
    len += (size_t)e->length;
  }

  uint64_t held_us;
  bool forced = false;
  if (guard_on)
    guard_wait(idr, &held_us);
  else
    pace_wait(&held_us, &forced);
  int rc = write_record(false, au_buf, (uint32_t)len, idr, VIDEO_WAIT_MS);
  if (rc == 0)
    pace_written++;
  if (rc == -EOPNOTSUPP) {
    pthread_mutex_lock(&ring_mx);
    printf("[core] lo stream non si puo' dare al core (%s)\n", h264ring_rewrite_why(ring));
    snprintf(refused_why, sizeof(refused_why), "Stream non adatto al core: %s", h264ring_rewrite_why(ring));
    cavlc_refused = true;
    pthread_mutex_unlock(&ring_mx);
    if (refused_cb)
      refused_cb(refused_why);
    n.refusals++;
    n.au_dropped++;
    return DR_OK;
  }
  if (rc == -EBADMSG) {
    pthread_mutex_lock(&ring_mx);
    printf("[core] access unit %lu non riscritta (%s)\n", n.au, h264ring_rewrite_why(ring));
    need_idr = true;
    pthread_mutex_unlock(&ring_mx);
    n.au_dropped++;
    return DR_NEED_IDR;
  }
  if (rc) {
    pthread_mutex_lock(&ring_mx);
    if (rc != -ENOTCONN && !gone)
      printf("[core] access unit %lu non scritta (%s)\n", n.au, strerror(-rc));
    need_idr = true;
    poll_status_locked();
    pthread_mutex_unlock(&ring_mx);
    n.au_dropped++;
    return DR_NEED_IDR;
  }
  n.au++;
  n.au_bytes += len;

  ritmo_au(du, held_us, forced);
  pthread_mutex_lock(&ring_mx);
  {
    struct h264ring_cavlc_stats cs;
    h264ring_rewrite_stats(ring, &cs);
    if (cs.units_rewritten && !n.au_rewritten)
      printf("[core] lo stream e' CAVLC: riscrittura in CABAC sull'ARM (lenta: disattiva CAVLC in Sunshine)\n");
    n.au_rewritten = (unsigned long)cs.units_rewritten;
  }
  poll_status_locked();
  pthread_mutex_unlock(&ring_mx);
  return DR_OK;
}

DECODER_RENDERER_CALLBACKS mg_video_callbacks = {
  .setup = video_setup,
  .cleanup = video_cleanup,
  .submitDecodeUnit = video_submit,
  .capabilities = 0,
};

/* ---------- audio: Opus -> PCM 48 kHz stereo al core ---------- */

static int audio_init(int audioConfiguration, POPUS_MULTISTREAM_CONFIGURATION cfg, void *context, int arFlags) {
  if (cfg->sampleRate != H264RING_AUD_RATE) {
    printf("[core] audio a %d Hz: il core suona a 48 kHz\n", cfg->sampleRate);
    return -1;
  }
  int err;
  opus = opus_multistream_decoder_create(cfg->sampleRate, cfg->channelCount, cfg->streams, cfg->coupledStreams,
                                         cfg->mapping, &err);
  if (!opus) {
    printf("[core] decoder Opus: errore %d\n", err);
    return -1;
  }
  pcm_frames = cfg->samplesPerFrame > 0 ? cfg->samplesPerFrame : 960;
  pcm_channels = cfg->channelCount;
  pcm = malloc(sizeof(int16_t) * (size_t)pcm_channels * (size_t)pcm_frames);
  pcm2 = malloc(sizeof(int16_t) * 2 * (size_t)pcm_frames);
  return pcm && pcm2 ? 0 : -1;
}

static void audio_cleanup(void) {
  if (opus)
    opus_multistream_decoder_destroy(opus);
  opus = NULL;
  free(pcm);
  free(pcm2);
  pcm = pcm2 = NULL;
}

static void audio_sample(char *data, int length) {
  /* data NULL: pacchetto perso, Opus lo maschera e la coda del core resta piena */
  int frames = opus_multistream_decode(opus, (const unsigned char *)data, length, pcm, pcm_frames, 0);
  if (frames <= 0) {
    n.aud_dropped++;
    return;
  }
  if (!data)
    n.aud_plc++;
  const int16_t *out = pcm;
  if (pcm_channels != 2) {
    for (int i = 0; i < frames; i++) {
      pcm2[2 * i] = pcm[i * pcm_channels];
      pcm2[2 * i + 1] = pcm[i * pcm_channels + (pcm_channels > 1 ? 1 : 0)];
    }
    out = pcm2;
  }
  int rc = write_record(true, out, (uint32_t)frames * 4u, false, AUDIO_WAIT_MS);
  if (rc == 0)
    n.aud++;
  else if (rc == W_EARLY)
    n.aud_early++;
  else
    n.aud_dropped++;
}

AUDIO_RENDERER_CALLBACKS mg_audio_callbacks = {
  .init = audio_init,
  .cleanup = audio_cleanup,
  .decodeAndPlaySample = audio_sample,
  .capabilities = CAPABILITY_DIRECT_SUBMIT,
};

void mg_core_get_counters(mg_core_counters *c) { *c = n; }

void mg_core_print_counters(void) {
  printf("[core] %lu access unit (%lu byte, %lu riscritte da CAVLC), %lu scartate; %lu errori del decoder, "
         "%lu IDR chiesti; %lu sessioni; audio %lu blocchi, %lu scartati, %lu prima del video, %lu mascherati\n",
         n.au, n.au_bytes, n.au_rewritten, n.au_dropped, n.errors_seen, n.idr_asked, n.sessions, n.aud,
         n.aud_dropped, n.aud_early, n.aud_plc);
}
