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

/* overlay */
static struct h264ring_fb fb;
static bool fb_ok;
static unsigned ovl_fmt, ovl_bpp, ovl_pages, ovl_page;
static uint32_t ovl_page_bytes;
static int ovl_gen, ovl_w, ovl_h;
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
      ci->refresh_x100 = std == 2 ? 4999 : 5991;
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
  struct h264ring_caps caps;
  h264ring_caps(&st, &caps);
  ci->max_mbs = caps.max_mbs;
  ci->mb_rate = caps.mb_rate;
}

/* ---------- overlay ---------- */

static bool fb_mapped; /* da h264ring_fb_map (non un framebuffer dei test) */

static void overlay_layout(void) {
  ovl_w = 640;
  ovl_h = 480;
  static const struct { unsigned fmt, bpp, pages; } tries[] = {
    { H264RING_OVL_FMT_8888, 4, 2 }, { H264RING_OVL_FMT_1555, 2, 2 },
    { H264RING_OVL_FMT_8888, 4, 1 }, { H264RING_OVL_FMT_1555, 2, 1 },
  };
  for (size_t i = 0; i < sizeof(tries) / sizeof(tries[0]); i++) {
    uint32_t pb = (uint32_t)ovl_w * tries[i].bpp * (uint32_t)ovl_h;
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
  printf("[core] overlay %dx%d, %u byte per pixel, %u pagine (framebuffer %u byte)\n", ovl_w, ovl_h, ovl_bpp,
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

int mg_core_overlay_present(const uint32_t *canvas, int w, int h) {
  if (!fb_ok || !ring || w * 2 != ovl_w || h * 2 != ovl_h)
    return -1;
  /* la pagina da disegnare non deve essere quella che il core sta leggendo */
  unsigned page = ovl_pages > 1 ? ovl_page ^ 1 : 0;
  if (ovl_pages > 1 && ovl_gen > 0) {
    pthread_mutex_lock(&ring_mx);
    h264ring_overlay_wait(ring, ovl_gen, 50);
    pthread_mutex_unlock(&ring_mx);
  }
  uint8_t *base = (uint8_t *)fb.virt + (size_t)page * ovl_page_bytes;
  size_t stride = (size_t)ovl_w * ovl_bpp;
  if (ovl_bpp == 4) {
    uint32_t line[1280];
    for (int y = 0; y < h; y++) {
      const uint32_t *src = canvas + (size_t)y * w;
      for (int x = 0; x < w; x++) {
        uint32_t c = src[x] | 0xFF000000u; /* B, G, R, A in memoria = 0xAARRGGBB */
        line[2 * x] = c;
        line[2 * x + 1] = c;
      }
      memcpy(base + (size_t)(2 * y) * stride, line, stride);
      memcpy(base + (size_t)(2 * y + 1) * stride, line, stride);
    }
  } else {
    uint16_t line[1280];
    for (int y = 0; y < h; y++) {
      const uint32_t *src = canvas + (size_t)y * w;
      for (int x = 0; x < w; x++) {
        uint32_t c = src[x];
        uint16_t v = 0x8000 | (((c >> 19) & 0x1F) << 10) | (((c >> 11) & 0x1F) << 5) | ((c >> 3) & 0x1F);
        line[2 * x] = v;
        line[2 * x + 1] = v;
      }
      memcpy(base + (size_t)(2 * y) * stride, line, stride);
      memcpy(base + (size_t)(2 * y + 1) * stride, line, stride);
    }
  }
  __sync_synchronize();
  struct h264ring_overlay o;
  memset(&o, 0, sizeof(o));
  o.base = fb.phys + page * ovl_page_bytes;
  o.width = (uint16_t)ovl_w;
  o.height = (uint16_t)ovl_h;
  o.stride = (uint16_t)stride;
  o.format = (uint8_t)ovl_fmt;
  o.band_first = 0;
  o.band_last = (uint16_t)(ovl_h - 1);
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

int mg_core_open(void (*on_gone)(void), void (*on_refused)(const char *why)) {
  printf("[core] carico il core H264...\n");
  int rc = h264ring_launch_core(NULL, LAUNCH_MS);
  if (rc) {
    printf("[core] il core H264 non parte (%s): serve _Other/H264_*.rbf sulla SD\n", strerror(-rc));
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
  streaming = true;
  pthread_mutex_unlock(&ring_mx);
}

void mg_core_stream_end(void) {
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

  int rc = write_record(false, au_buf, (uint32_t)len, idr, VIDEO_WAIT_MS);
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
