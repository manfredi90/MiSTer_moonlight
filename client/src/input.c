/*
 * Input: pad, tastiera e mouse del MiSTer. Mentre gira un core il Main tiene per sé
 * tutti i dispositivi di /dev/input; il core H264 copia quello che il Main gli manda
 * (pad, tasti PS/2 set 2, somme del mouse, apertura dell'OSD) nel suo blocco di
 * controllo, e qui lo leggiamo (h264ring_input) per mandarlo a Sunshine o al menu.
 *
 * - Tasti: PS/2 set 2 (+E0) -> codice Linux (ps2_keys.h, la tabella inversa del Main)
 *   -> Virtual Key di Windows. Un rilascio senza pressione si ignora (NumLock, un tasto
 *   tenuto mentre l'OSD era aperto). Pausa arriva una volta sola: giù e su insieme.
 * - Mouse: le differenze delle somme dall'ultima lettura.
 * - OSD del MiSTer aperto: si rilascia tutto e non si inoltra niente fino alla chiusura.
 *   Le sue azioni: "Send Home to the program" = tasto Guide, "Program menu" = torna
 *   al nostro menu.
 * - Vibrazione: il callback di Sunshine salva il valore, questo thread lo scrive al
 *   core (h264ring_rumble) e lo tiene vivo oltre i 32 s del Main.
 * - Modalità mouse col pad: Start tenuto da solo 1 s la attiva/disattiva.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */
#define _GNU_SOURCE
#include "input.h"
#include "core.h"
#include "ps2_keys.h"

#include "h264ring.h"

#include <Limelight.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define NPADS 6
#define MAX_HOST_PADS 4
#define RUMBLE_KEEPALIVE_MS 100
#define GUIDE_TAP_S 0.1

#define P_UP H264RING_PAD_UP
#define P_DOWN H264RING_PAD_DOWN
#define P_LEFT H264RING_PAD_LEFT
#define P_RIGHT H264RING_PAD_RIGHT
#define P_DIRS (P_UP | P_DOWN | P_LEFT | P_RIGHT)

static pthread_t thread;
static volatile bool running;
static int num_pads = 1;
static void (*quit_cb)(void);
static bool debug, stick_dpad;
static volatile int mode = MG_INPUT_MENU;

void mg_input_set_debug(bool on) { debug = on; }
void mg_input_set_stick_dpad(bool on) { stick_dpad = on; }

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* codice tasto Linux -> Virtual Key di Windows */
static const uint8_t lin2vk[128] = {
  [1] = 0x1B, [2] = '1', [3] = '2', [4] = '3', [5] = '4', [6] = '5', [7] = '6', [8] = '7', [9] = '8',
  [10] = '9', [11] = '0', [12] = 0xBD, [13] = 0xBB, [14] = 0x08, [15] = 0x09,
  [16] = 'Q', [17] = 'W', [18] = 'E', [19] = 'R', [20] = 'T', [21] = 'Y', [22] = 'U', [23] = 'I',
  [24] = 'O', [25] = 'P', [26] = 0xDB, [27] = 0xDD, [28] = 0x0D, [29] = 0xA2,
  [30] = 'A', [31] = 'S', [32] = 'D', [33] = 'F', [34] = 'G', [35] = 'H', [36] = 'J', [37] = 'K',
  [38] = 'L', [39] = 0xBA, [40] = 0xDE, [41] = 0xC0, [42] = 0xA0, [43] = 0xDC,
  [44] = 'Z', [45] = 'X', [46] = 'C', [47] = 'V', [48] = 'B', [49] = 'N', [50] = 'M',
  [51] = 0xBC, [52] = 0xBE, [53] = 0xBF, [54] = 0xA1, [55] = 0x6A, [56] = 0xA4, [57] = 0x20,
  [58] = 0x14, [59] = 0x70, [60] = 0x71, [61] = 0x72, [62] = 0x73, [63] = 0x74, [64] = 0x75,
  [65] = 0x76, [66] = 0x77, [67] = 0x78, [68] = 0x79, [69] = 0x90, [70] = 0x91,
  [71] = 0x67, [72] = 0x68, [73] = 0x69, [74] = 0x6D, [75] = 0x64, [76] = 0x65, [77] = 0x66,
  [78] = 0x6B, [79] = 0x61, [80] = 0x62, [81] = 0x63, [82] = 0x60, [83] = 0x6E,
  [86] = 0xE2, [87] = 0x7A, [88] = 0x7B, [96] = 0x0D, [97] = 0xA3, [98] = 0x6F, [99] = 0x2C,
  [100] = 0xA5, [102] = 0x24, [103] = 0x26, [104] = 0x21, [105] = 0x25, [106] = 0x27,
  [107] = 0x23, [108] = 0x28, [109] = 0x22, [110] = 0x2D, [111] = 0x2E, [119] = 0x13,
  [125] = 0x5B, [126] = 0x5C, [127] = 0x5D,
};

static unsigned lin_key(unsigned ext, unsigned code) { return ps2_linux[ext & 1][code & 0xFF]; }
static unsigned vk_of(unsigned ext, unsigned code) {
  unsigned k = lin_key(ext, code);
  return k < 128 ? lin2vk[k] : 0;
}

static char modifier_of(unsigned lk) {
  switch (lk) {
  case 42: case 54: return MODIFIER_SHIFT;
  case 29: case 97: return MODIFIER_CTRL;
  case 56: case 100: return MODIFIER_ALT;
  case 125: case 126: return MODIFIER_META;
  default: return 0;
  }
}

/* ---------- stato ---------- */

static bool started;
static uint32_t ev_count, dx0, dy0, wh0;
static bool osd;
static bool held[2][256];
static char modifiers;
static bool quit_armed;
static uint8_t mbtn;            /* tasti del mouse premuti sull'host */
static uint64_t pad_raw[NPADS];  /* ultima lettura dei pad */
static int pad_sent_flags[MAX_HOST_PADS];
static short pad_sent_axes[MAX_HOST_PADS][4];
static unsigned char pad_sent_trig[MAX_HOST_PADS][2];
static bool pads_announced;
static double guide_until; /* tasto Guide tenuto fino a (dall'OSD) */
static unsigned actions;

/* vibrazione */
static uint16_t rum_want[NPADS];  /* {forte, debole}, scritto dal callback */
static uint16_t rum_sent[NPADS];
static unsigned rum_ms;
static double pulse_until;

/* modalità mouse */
static bool mouse_mode;
static double start_since;
static bool start_used;
static uint8_t mm_btn;
static double acc_x, acc_y, acc_sx, acc_sy;

/* menu */
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cond = PTHREAD_COND_INITIALIZER;
static int keyq[32], kq_head, kq_count;
static unsigned menu_prev_pads;
static int menu_held;
static double menu_next_repeat;

static short host_mask(void) { return (short)((1 << num_pads) - 1); }

static void push_key(int k) {
  pthread_mutex_lock(&q_lock);
  if (kq_count < 32) {
    keyq[(kq_head + kq_count) % 32] = k;
    kq_count++;
    pthread_cond_signal(&q_cond);
  }
  pthread_mutex_unlock(&q_lock);
}

int mg_input_menu_key(int timeout_ms) {
  pthread_mutex_lock(&q_lock);
  if (!kq_count && timeout_ms > 0) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_cond_timedwait(&q_cond, &q_lock, &ts);
  }
  int k = MG_KEY_NONE;
  if (kq_count) {
    k = keyq[kq_head];
    kq_head = (kq_head + 1) % 32;
    kq_count--;
  }
  pthread_mutex_unlock(&q_lock);
  return k;
}

/* ---------- vibrazione ---------- */

void mg_rumble(unsigned short n, unsigned short low, unsigned short high) {
  if (n >= NPADS)
    return;
  __atomic_store_n(&rum_want[n], (uint16_t)((low & 0xFF00u) | (high >> 8)), __ATOMIC_RELAXED);
}

void mg_rumble_stop_all(void) {
  for (int n = 0; n < NPADS; n++)
    __atomic_store_n(&rum_want[n], 0, __ATOMIC_RELAXED);
}

static void rumble_tick(struct h264ring *r, unsigned ms) {
  bool pulse = pulse_until > 0 && now_s() < pulse_until;
  if (pulse_until > 0 && !pulse)
    pulse_until = 0;
  for (int n = 0; n < NPADS; n++) {
    uint16_t v = __atomic_load_n(&rum_want[n], __ATOMIC_RELAXED);
    if (n == 0 && pulse)
      v = 0x6060;
    if (v != rum_sent[n] && h264ring_rumble(r, (unsigned)n, v >> 8, v & 0xFFu) == 0)
      rum_sent[n] = v;
  }
  rum_ms += ms;
  if (rum_ms >= RUMBLE_KEEPALIVE_MS) {
    rum_ms = 0;
    h264ring_rumble_keepalive(r);
  }
}

static void feedback_pulse(void) { pulse_until = now_s() + 0.15; }

/* ---------- verso l'host ---------- */

static void send_key(unsigned ext, unsigned code, bool down) {
  unsigned vk = vk_of(ext, code);
  if (debug)
    printf("[input] tasto PS/2 %s%02X -> VK 0x%02X %s\n", ext ? "E0 " : "", code, vk, down ? "giu'" : "su");
  LiSendKeyboardEvent2((short)(0x8000 | vk), down ? KEY_ACTION_DOWN : KEY_ACTION_UP,
                       modifiers | (ext ? MODIFIER_EXTENDED : 0), 0);
}

static void send_pad(int p, int flags, unsigned char lt, unsigned char rt, short lx, short ly, short rx, short ry) {
  if (p >= num_pads)
    return;
  if (pad_sent_flags[p] == flags && pad_sent_trig[p][0] == lt && pad_sent_trig[p][1] == rt &&
      pad_sent_axes[p][0] == lx && pad_sent_axes[p][1] == ly && pad_sent_axes[p][2] == rx && pad_sent_axes[p][3] == ry)
    return;
  pad_sent_flags[p] = flags;
  pad_sent_trig[p][0] = lt;
  pad_sent_trig[p][1] = rt;
  pad_sent_axes[p][0] = lx;
  pad_sent_axes[p][1] = ly;
  pad_sent_axes[p][2] = rx;
  pad_sent_axes[p][3] = ry;
  if (debug)
    printf("[input] pad %d: pulsanti 0x%06x LT %d RT %d L(%d,%d) R(%d,%d)\n", p + 1, flags, lt, rt, lx, ly, rx, ry);
  LiSendMultiControllerEvent((short)p, host_mask(), flags, lt, rt, lx, ly, rx, ry);
}

static void mouse_buttons_to(uint8_t *state, uint8_t now) {
  static const int map[5] = { BUTTON_LEFT, BUTTON_RIGHT, BUTTON_MIDDLE, BUTTON_X1, BUTTON_X2 };
  for (int i = 0; i < 5; i++) {
    bool was = (*state >> i) & 1, is = (now >> i) & 1;
    if (was != is)
      LiSendMouseButtonEvent(is ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE, map[i]);
  }
  *state = now;
}

static void release_all(void) {
  for (unsigned ext = 0; ext < 2; ext++)
    for (unsigned code = 0; code < 256; code++)
      if (held[ext][code]) {
        held[ext][code] = false;
        modifiers &= ~modifier_of(lin_key(ext, code));
        send_key(ext, code, false);
      }
  modifiers = 0;
  quit_armed = false;
  mouse_buttons_to(&mbtn, 0);
  mouse_buttons_to(&mm_btn, 0);
  for (int p = 0; p < num_pads; p++)
    send_pad(p, 0, 0, 0, 0, 0, 0, 0);
  guide_until = 0;
}

static void announce_pads(void) {
  for (int p = 0; p < num_pads; p++) {
    LiSendControllerArrivalEvent((uint8_t)p, host_mask(), LI_CTYPE_XBOX,
                                 A_FLAG | B_FLAG | X_FLAG | Y_FLAG | UP_FLAG | DOWN_FLAG | LEFT_FLAG | RIGHT_FLAG |
                                 LB_FLAG | RB_FLAG | PLAY_FLAG | BACK_FLAG | LS_CLK_FLAG | RS_CLK_FLAG | SPECIAL_FLAG,
                                 LI_CCAP_RUMBLE);
    pad_sent_flags[p] = -1; /* il prossimo stato si manda comunque */
  }
  pads_announced = true;
}

/* ---------- tasti ---------- */

static void key_stream(unsigned ext, unsigned code, bool pressed) {
  unsigned lk = lin_key(ext, code);
  if (!vk_of(ext, code))
    return;
  if (lk == 119) { /* Pausa: il Main la manda una volta, al rilascio */
    send_key(ext, code, true);
    send_key(ext, code, false);
    return;
  }
  if (!pressed && !held[ext][code])
    return;
  held[ext][code] = pressed;
  char mod = modifier_of(lk);
  if (pressed) modifiers |= mod; else modifiers &= ~mod;
  /* Ctrl+Alt+Shift+Q: chiude lo stream quando i modificatori tornano su */
  if (pressed && lk == 16 && (modifiers & (MODIFIER_CTRL | MODIFIER_ALT | MODIFIER_SHIFT)) ==
                                 (MODIFIER_CTRL | MODIFIER_ALT | MODIFIER_SHIFT)) {
    quit_armed = true;
    held[ext][code] = false; /* Q non arriva al PC, né giù né su */
    return;
  }
  send_key(ext, code, pressed);
  if (quit_armed && modifiers == 0) {
    quit_armed = false;
    printf("[input] Ctrl+Alt+Shift+Q: fine dello stream\n");
    release_all();
    if (quit_cb) quit_cb();
  }
}

static void key_menu(unsigned ext, unsigned code, bool pressed) {
  if (!pressed)
    return;
  if (ext) {
    switch (code) {
    case 0x75: push_key(MG_KEY_UP); break;
    case 0x72: push_key(MG_KEY_DOWN); break;
    case 0x6B: push_key(MG_KEY_LEFT); break;
    case 0x74: push_key(MG_KEY_RIGHT); break;
    case 0x5A: push_key(MG_KEY_OK); break;
    }
  } else {
    switch (code) {
    case 0x5A: case 0x29: push_key(MG_KEY_OK); break;
    case 0x76: case 0x66: push_key(MG_KEY_BACK); break;
    }
  }
}

/* ---------- modalità mouse ---------- */

static double stick_curve(int v) {
  double x = v / 127.0;
  const double dz = 0.15;
  if (x > -dz && x < dz) return 0;
  double m = (x > 0 ? x - dz : -x - dz) / (1 - dz);
  if (m > 1) m = 1;
  m = m * m;
  return x > 0 ? m : -m;
}

static void set_mouse_mode(bool on) {
  mouse_mode = on;
  send_pad(0, 0, 0, 0, 0, 0, 0, 0);
  mouse_buttons_to(&mm_btn, 0);
  acc_x = acc_y = acc_sx = acc_sy = 0;
  feedback_pulse();
  printf("[input] modalita' mouse %s\n", on ? "attiva" : "disattivata");
}

/* Start tenuto da solo 1 s commuta la modalità; ritorna true se il pad 1 fa da mouse */
static bool mouse_pad1(uint64_t w, unsigned ms) {
  unsigned b = (unsigned)w & 0xFFFFu;
  double t = now_s();
  if (b == H264RING_PAD_START) {
    if (start_since == 0) {
      start_since = t;
      start_used = false;
    } else if (!start_used && t - start_since >= 1.0) {
      start_used = true;
      set_mouse_mode(!mouse_mode);
    }
  } else {
    start_since = 0;
  }
  if (!mouse_mode)
    return false;
  if (b & H264RING_PAD_BACK) /* le combinazioni con Select passano al pad normale */
    return false;
  int8_t lx = (int8_t)(w >> 32), ly = (int8_t)(w >> 40), rx = (int8_t)(w >> 48), ry = (int8_t)(w >> 56);
  double dt = ms / 1000.0;
  if (dt > 0.05) dt = 0.05;
  double vx = stick_curve(lx), vy = stick_curve(ly);
  unsigned d = b;
  if (lx < -24 || lx > 24 || ly < -24 || ly > 24)
    d = 0; /* le direzioni che il MiSTer ricava dalla levetta */
  if (d & P_LEFT) vx = -0.5;
  if (d & P_RIGHT) vx = 0.5;
  if (d & P_UP) vy = -0.5;
  if (d & P_DOWN) vy = 0.5;
  acc_x += vx * 1400 * dt;
  acc_y += vy * 1400 * dt;
  int dx = (int)acc_x, dy = (int)acc_y;
  if (dx || dy) {
    LiSendMouseMoveEvent((short)dx, (short)dy);
    acc_x -= dx;
    acc_y -= dy;
  }
  acc_sy += -stick_curve(ry) * 1200 * dt;
  acc_sx += stick_curve(rx) * 1200 * dt;
  int sy = (int)acc_sy, sx = (int)acc_sx;
  if (sy) { LiSendHighResScrollEvent((short)sy); acc_sy -= sy; }
  if (sx) { LiSendHighResHScrollEvent((short)sx); acc_sx -= sx; }
  uint8_t mb = 0;
  if (b & H264RING_PAD_A) mb |= 1;
  if (b & H264RING_PAD_B) mb |= 2;
  if (b & H264RING_PAD_X) mb |= 4;
  mouse_buttons_to(&mm_btn, mb);
  return true;
}

/* ---------- pad ---------- */

static short axis(int8_t v, bool invert) {
  int x = v * 258;
  if (invert) x = -x;
  if (x > 32767) x = 32767;
  if (x < -32768) x = -32768;
  return (short)x;
}

static void pads_stream(unsigned ms) {
  bool pad1_mouse = mouse_pad1(pad_raw[0], ms);
  for (int p = 0; p < num_pads; p++) {
    if (p == 0 && pad1_mouse)
      continue;
    uint64_t w = pad_raw[p];
    unsigned j = (unsigned)w & 0xFFFFu;
    int8_t lx = (int8_t)(w >> 32), ly = (int8_t)(w >> 40), rx = (int8_t)(w >> 48), ry = (int8_t)(w >> 56);
    if (!stick_dpad) {
      const int th = 24;
      if (ly < -th) j &= ~P_UP;
      if (ly > th) j &= ~P_DOWN;
      if (lx < -th) j &= ~P_LEFT;
      if (lx > th) j &= ~P_RIGHT;
    }
    int b = 0;
    if (j & P_UP) b |= UP_FLAG;
    if (j & P_DOWN) b |= DOWN_FLAG;
    if (j & P_LEFT) b |= LEFT_FLAG;
    if (j & P_RIGHT) b |= RIGHT_FLAG;
    if (j & H264RING_PAD_A) b |= A_FLAG;
    if (j & H264RING_PAD_B) b |= B_FLAG;
    if (j & H264RING_PAD_X) b |= X_FLAG;
    if (j & H264RING_PAD_Y) b |= Y_FLAG;
    if (j & H264RING_PAD_LB) b |= LB_FLAG;
    if (j & H264RING_PAD_RB) b |= RB_FLAG;
    if (j & H264RING_PAD_BACK) b |= BACK_FLAG;
    if (j & H264RING_PAD_START) b |= PLAY_FLAG;
    if (j & H264RING_PAD_LS) b |= LS_CLK_FLAG;
    if (j & H264RING_PAD_RS) b |= RS_CLK_FLAG;
    unsigned char lt = (j & H264RING_PAD_LT) ? 0xFF : 0, rt = (j & H264RING_PAD_RT) ? 0xFF : 0;
    /* Select + Start + LB + RB = chiudi lo stream */
    if ((b & (BACK_FLAG | PLAY_FLAG | LB_FLAG | RB_FLAG)) == (BACK_FLAG | PLAY_FLAG | LB_FLAG | RB_FLAG)) {
      printf("[input] Select+Start+LB+RB: fine dello stream\n");
      release_all();
      if (quit_cb) quit_cb();
      return;
    }
    /* Select + Start = tasto Guide */
    if ((b & (BACK_FLAG | PLAY_FLAG)) == (BACK_FLAG | PLAY_FLAG) && !(b & (LB_FLAG | RB_FLAG))) {
      b &= ~(BACK_FLAG | PLAY_FLAG);
      b |= SPECIAL_FLAG;
    }
    if (p == 0 && guide_until > 0 && now_s() < guide_until)
      b |= SPECIAL_FLAG;
    send_pad(p, b, lt, rt, axis(lx, false), axis(ly, true), axis(rx, false), axis(ry, true));
  }
}

static void pads_menu(void) {
  unsigned j = 0;
  for (int p = 0; p < NPADS; p++)
    j |= (unsigned)pad_raw[p] & 0xFFFFu;
  unsigned pressed = j & ~menu_prev_pads;
  menu_prev_pads = j;
  if (pressed & P_UP) push_key(MG_KEY_UP);
  if (pressed & P_DOWN) push_key(MG_KEY_DOWN);
  if (pressed & P_LEFT) push_key(MG_KEY_LEFT);
  if (pressed & P_RIGHT) push_key(MG_KEY_RIGHT);
  if (pressed & (H264RING_PAD_A | H264RING_PAD_START)) push_key(MG_KEY_OK);
  if (pressed & H264RING_PAD_B) push_key(MG_KEY_BACK);
  int dir = (j & P_UP) ? MG_KEY_UP : (j & P_DOWN) ? MG_KEY_DOWN : (j & P_LEFT) ? MG_KEY_LEFT :
            (j & P_RIGHT) ? MG_KEY_RIGHT : 0;
  double t = now_s();
  if (dir != menu_held) {
    menu_held = dir;
    menu_next_repeat = t + 0.4;
  } else if (dir && t >= menu_next_repeat) {
    push_key(dir);
    menu_next_repeat = t + 0.12;
  }
}

/* ---------- ciclo ---------- */

void mg_input_poll_once(unsigned ms) {
  struct h264ring *r = mg_core_ring();
  if (!r)
    return;
  struct h264ring_input in;
  if (h264ring_input(r, &in) != 0) {
    rumble_tick(r, ms);
    return;
  }
  int m = mode;
  if (!started) {
    started = true;
    ev_count = (uint32_t)H264RING_IN(&in, EV_COUNT);
    dx0 = (uint32_t)H264RING_IN(&in, DX_SUM);
    dy0 = (uint32_t)H264RING_IN(&in, DY_SUM);
    wh0 = (uint32_t)H264RING_IN(&in, WHEEL_SUM);
    osd = H264RING_IN(&in, OSD_OPEN) != 0;
    for (int p = 0; p < NPADS; p++)
      pad_raw[p] = in.w[H264RING_PAD0_JOY_W - H264RING_IN_WORD + p] & 0xFFFFFFFF0000FFFFull;
    menu_prev_pads = 0;
    for (int p = 0; p < NPADS; p++)
      menu_prev_pads |= (unsigned)pad_raw[p] & 0xFFFFu; /* tasti già premuti all'avvio: non contano */
  }
  if (guide_until > 0 && now_s() >= guide_until)
    guide_until = 0;

  uint16_t ev[H264RING_IN_Q_LEN];
  uint32_t lost = 0;
  unsigned k;
  while ((k = h264ring_input_events(&in, &ev_count, ev, H264RING_IN_Q_LEN, &lost)) != 0) {
    if (lost) {
      printf("[input] %u eventi persi: rilascio tutto\n", lost);
      lost = 0;
      if (m == MG_INPUT_STREAM)
        release_all();
      memset(held, 0, sizeof(held));
    }
    for (unsigned i = 0; i < k; i++) {
      unsigned t = H264RING_EV_TYPE(ev[i]);
      if (t == H264RING_EV_OSD) {
        bool open = H264RING_EV_OPEN(ev[i]);
        if (open && !osd) {
          if (m == MG_INPUT_STREAM)
            release_all();
          osd = true;
          printf("[input] OSD del MiSTer aperto: input in pausa\n");
        } else if (!open && osd) {
          osd = false;
          printf("[input] OSD del MiSTer chiuso\n");
        }
      } else if (t == H264RING_EV_ACTION) {
        actions |= 1u << (H264RING_EV_CODE(ev[i]) & 31);
      } else if (osd) {
        continue;
      } else if (t == H264RING_EV_KEY) {
        unsigned ext = H264RING_EV_EXT(ev[i]), code = H264RING_EV_CODE(ev[i]);
        bool pr = H264RING_EV_PRESSED(ev[i]);
        if (m == MG_INPUT_STREAM)
          key_stream(ext, code, pr);
        else
          key_menu(ext, code, pr);
      } else if (t == H264RING_EV_MOUSE_BTN) {
        if (m == MG_INPUT_STREAM)
          mouse_buttons_to(&mbtn, (uint8_t)(H264RING_EV_CODE(ev[i]) & 0x1F));
      }
    }
  }
  /* mouse: differenze delle somme (Y del PS/2 verso l'alto, dell'host verso il basso) */
  {
    int32_t dx = (int32_t)((uint32_t)H264RING_IN(&in, DX_SUM) - dx0);
    int32_t dy = -(int32_t)((uint32_t)H264RING_IN(&in, DY_SUM) - dy0);
    int32_t dw = -(int32_t)((uint32_t)H264RING_IN(&in, WHEEL_SUM) - wh0);
    dx0 = (uint32_t)H264RING_IN(&in, DX_SUM);
    dy0 = (uint32_t)H264RING_IN(&in, DY_SUM);
    wh0 = (uint32_t)H264RING_IN(&in, WHEEL_SUM);
    if (m == MG_INPUT_STREAM && !osd) {
      while (dx || dy) {
        short sx = dx > 32767 ? 32767 : dx < -32767 ? -32767 : (short)dx;
        short sy = dy > 32767 ? 32767 : dy < -32767 ? -32767 : (short)dy;
        LiSendMouseMoveEvent(sx, sy);
        dx -= sx;
        dy -= sy;
      }
      while (dw) {
        signed char c = dw > 127 ? 127 : dw < -127 ? -127 : (signed char)dw;
        LiSendScrollEvent(c);
        dw -= c;
      }
    }
  }
  for (int p = 0; p < NPADS; p++)
    pad_raw[p] = in.w[H264RING_PAD0_JOY_W - H264RING_IN_WORD + p] & 0xFFFFFFFF0000FFFFull;

  /* azioni dell'OSD, dopo la chiusura */
  if (actions && !osd) {
    if (m == MG_INPUT_STREAM && (actions & 1u)) {
      printf("[input] OSD: Send Home -> tasto Guide\n");
      guide_until = now_s() + GUIDE_TAP_S;
    }
    if (m == MG_INPUT_STREAM && (actions & 2u)) {
      printf("[input] OSD: Program menu -> torno al menu\n");
      release_all();
      if (quit_cb) quit_cb();
    }
    actions = 0;
  }
  if (!osd) {
    if (m == MG_INPUT_STREAM) {
      if (!pads_announced)
        announce_pads();
      pads_stream(ms);
    } else {
      pads_menu();
    }
  }
  rumble_tick(r, ms); /* dopo i pad: l'impulso della modalità mouse parte subito */
}

static void *loop(void *arg) {
  double last = now_s(), last_poll = 0;
  while (running) {
    usleep(mode == MG_INPUT_STREAM ? 2000 : 5000);
    double t = now_s();
    unsigned ms = (unsigned)((t - last) * 1000 + 0.5);
    last = t;
    mg_input_poll_once(ms ? ms : 1);
    if (mode == MG_INPUT_STREAM)
      mg_core_tick();
    if (t - last_poll >= 0.05) {
      last_poll = t;
      mg_core_poll();
    }
  }
  return NULL;
}

void mg_input_set_mode(int m) {
  if (m == MG_INPUT_STREAM) {
    pads_announced = false;
    mouse_mode = false;
    start_since = 0;
    mm_btn = mbtn = 0;
    memset(held, 0, sizeof(held));
    modifiers = 0;
    quit_armed = false;
    actions = 0;
    guide_until = 0;
  } else {
    /* i tasti tenuti all'uscita dallo stream non devono agire nel menu */
    menu_prev_pads = 0;
    for (int p = 0; p < NPADS; p++)
      menu_prev_pads |= (unsigned)pad_raw[p] & 0xFFFFu;
    menu_held = 0;
    pthread_mutex_lock(&q_lock);
    kq_count = 0;
    pthread_mutex_unlock(&q_lock);
  }
  mode = m;
}

void mg_input_setup(int pads, void (*on_quit)(void)) {
  num_pads = pads < 1 ? 1 : pads > MAX_HOST_PADS ? MAX_HOST_PADS : pads;
  quit_cb = on_quit;
  started = false;
}

void mg_input_start(int pads, void (*on_quit)(void)) {
  mg_input_setup(pads, on_quit);
  running = true;
  pthread_create(&thread, NULL, loop, NULL);
}

void mg_input_stop(void) {
  if (!running)
    return;
  running = false;
  pthread_join(thread, NULL);
  struct h264ring *r = mg_core_ring();
  mg_rumble_stop_all();
  pulse_until = 0;
  if (r)
    for (int n = 0; n < NPADS; n++)
      if (rum_sent[n] && h264ring_rumble(r, (unsigned)n, 0, 0) == 0)
        rum_sent[n] = 0;
}
