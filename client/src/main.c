/*
 * MiSTer Moonlight: client Moonlight per Sunshine che gira sull'ARM del MiSTer FPGA
 * e usa il core H264 (decoder H.264 in FPGA) come uscita video e audio, con un
 * menu a schermo per scegliere l'app e le impostazioni.
 *
 * Basato su Moonlight Embedded (libgamestream, moonlight-common-c) e sulla libreria
 * del core H264 (third_party/h264ring, owenb321/MiSTer_h264_decoder).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */
#define _GNU_SOURCE
#include "core.h"
#include "input.h"
#include "netdiag.h"
#include "options.h"

#include "h264ring.h"

#include <Limelight.h>
#include <client.h>
#include <errors.h>

#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MG_VERSION "0.2.0"

static sem_t quit_sem;
static volatile bool stop_play;
static volatile bool exiting;
static char end_reason[160];
static volatile bool net_trouble;

static void request_quit(void) { stop_play = true; sem_post(&quit_sem); }
void mg_request_quit(void) { request_quit(); }
bool mg_exiting(void) { return exiting; }

static void on_signal(int sig) { exiting = true; stop_play = true; sem_post(&quit_sem); }

static void on_core_gone(void) {
  snprintf(end_reason, sizeof(end_reason), "Il core H264 non e' piu' attivo");
  exiting = true;
  stop_play = true;
  sem_post(&quit_sem);
}

static void on_refused(const char *why) {
  snprintf(end_reason, sizeof(end_reason), "%s", why);
  stop_play = true;
  sem_post(&quit_sem);
}

/* ---------- risoluzioni ---------- */

static const mg_res resolutions[] = {
  { 640, 480, "640x480 (4:3)" },
  { 720, 480, "720x480 (tutto il quadro)" },
  { 848, 480, "848x480 (16:9)" },
  { 512, 384, "512x384 (4:3)" },
  { 320, 240, "320x240 (4:3)" },
};
int mg_res_count(void) { return (int)(sizeof(resolutions) / sizeof(resolutions[0])); }
const mg_res *mg_res_at(int i) { return &resolutions[i]; }
int mg_res_find(int w, int h) {
  for (int i = 0; i < mg_res_count(); i++)
    if (resolutions[i].w == w && resolutions[i].h == h)
      return i;
  return -1;
}

/* ---------- callback di moonlight-common-c ---------- */

static void conn_terminated(int err) {
  if (err == ML_ERROR_GRACEFUL_TERMINATION)
    snprintf(end_reason, sizeof(end_reason), "Sessione chiusa dal PC");
  else if (err == ML_ERROR_NO_VIDEO_TRAFFIC)
    snprintf(end_reason, sizeof(end_reason), "Nessun video dal PC: controlla il firewall (UDP 47998-48000)");
  else
    snprintf(end_reason, sizeof(end_reason), "Connessione terminata (errore %d)", err);
  if (err != ML_ERROR_GRACEFUL_TERMINATION)
    net_trouble = true;
  printf("%s\n", end_reason);
  sem_post(&quit_sem);
}

static void conn_stage_failed(int stage, int err) {
  snprintf(end_reason, sizeof(end_reason), "Avvio fallito nella fase '%s' (errore %d)", LiGetStageName(stage), err);
  printf("%s\n", end_reason);
}

static void conn_log(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
}

static void conn_status(int s) {
  if (s == CONN_STATUS_POOR)
    printf("Connessione scarsa: prova ad abbassare il bitrate\n");
}

static CONNECTION_LISTENER_CALLBACKS conn_cb = {
  .stageFailed = conn_stage_failed,
  .connectionTerminated = conn_terminated,
  .logMessage = conn_log,
  .connectionStatusUpdate = conn_status,
  .rumble = mg_rumble,
};

/* ---------- opzioni ---------- */

static void usage(void) {
  printf("mister_moonlight %s - client Moonlight per Sunshine con uscita sul core H264 del MiSTer\n\n", MG_VERSION);
  printf("Uso: mister_moonlight <azione> <ip host> [opzioni]\n\n");
  printf("Azioni:\n");
  printf("  menu <ip>      menu a schermo: app di Sunshine, impostazioni, abbinamento\n");
  printf("  stream <ip>    avvia subito lo streaming di un'app (-app)\n");
  printf("  pair <ip>      abbina il MiSTer a Sunshine (il PIN va nella pagina web di Sunshine)\n");
  printf("  unpair <ip>    rimuove l'abbinamento\n");
  printf("  list <ip>      elenca le app configurate in Sunshine\n");
  printf("  quit <ip>      chiude l'app in esecuzione sul PC\n");
  printf("  play <file>    prova senza Sunshine: manda al core un file .h264 grezzo\n\n");
  printf("Opzioni:\n");
  printf("  -app <nome>        app di Sunshine (predefinita: Desktop)\n");
  printf("  -width <n> -height <n>  risoluzione chiesta a Sunshine (predefinita 640x480)\n");
  printf("  -bitrate <kbps>    bitrate (predefinito 10000)\n");
  printf("  -fps <n>           fps chiesti a Sunshine (predefiniti: i quadri del core, 59.94; 30 = la meta')\n");
  printf("  -pads <1-4>        pad da emulare sul PC (predefinito 1)\n");
  printf("  -noaudio           niente audio\n");
  printf("  -remote            ottimizza per streaming via internet\n");
  printf("  -packetsize <n>    dimensione pacchetti video (predefinita 1392)\n");
  printf("  -keydir <dir>      cartella delle chiavi di abbinamento\n");
  printf("  -quitappafter      chiude l'app sul PC all'uscita\n");
  printf("  -stats             contatori del core alla fine dello stream\n");
  printf("  -pin <nnnn>        PIN da usare con 'pair'\n");
  printf("  -cpu <n>           CPU su cui girare (predefinita: nessuna)\n");
  printf("  -stickdpad         lascia passare le direzioni digitali che il MiSTer ricava dalla levetta\n");
  printf("  -debuginput        stampa gli input inoltrati al PC\n");
  printf("  -nomenucfg         il menu non legge menu.cfg\n");
  printf("  -pacing            un'immagine al core solo dopo la presa della precedente (sperimentale)\n");
  printf("  -guard             guardia di fase: col core con la P3e, nessuna immagine a ridosso della presa\n");
  printf("  -genlock           genlock: il core accorda la sua fase agli arrivi (core di questo repository)\n");
  printf("  -core PERCORSO     il core da caricare, senza data ed estensione (predefinito _Other/H264)\n\n");
  printf("Durante lo stream: Select+Start+LB+RB o Ctrl+Alt+Shift+Q tornano al menu,\n");
  printf("Select+Start = tasto Guide, Start tenuto 1 s = modalita' mouse.\n");
}

static void default_keydir(char *out, size_t n) {
  char exe[PATH_MAX];
  ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (len > 0) {
    exe[len] = 0;
    char *slash = strrchr(exe, '/');
    if (slash) *slash = 0;
    snprintf(out, n, "%s/keys", exe);
  } else {
    snprintf(out, n, "keys");
  }
}

static int parse(int argc, char **argv, options *o) {
  memset(o, 0, sizeof(*o));
  o->app = "Desktop";
  o->width = 640;
  o->height = 480;
  o->pads = 1;
  o->audio = true;
  o->remote = STREAM_CFG_AUTO;
  o->packet_size = 1392;
  o->cpu = -1;
  if (argc < 2)
    return -1;
  o->action = argv[1];
  int i = 2;
  if (i < argc && argv[i][0] != '-')
    o->host = argv[i++];
  for (; i < argc; i++) {
    const char *a = argv[i];
    const char *v = i + 1 < argc ? argv[i + 1] : NULL;
#define NEEDV() do { if (!v) { fprintf(stderr, "%s richiede un valore\n", a); return -1; } i++; } while (0)
    if (!strcmp(a, "-app")) { NEEDV(); o->app = v; }
    else if (!strcmp(a, "-width")) { NEEDV(); o->width = atoi(v); }
    else if (!strcmp(a, "-height")) { NEEDV(); o->height = atoi(v); }
    else if (!strcmp(a, "-bitrate")) { NEEDV(); o->bitrate = atoi(v); }
    else if (!strcmp(a, "-fps")) { NEEDV(); o->fps = atoi(v); }
    else if (!strcmp(a, "-pads")) { NEEDV(); o->pads = atoi(v); }
    else if (!strcmp(a, "-noaudio")) o->audio = false;
    else if (!strcmp(a, "-remote")) o->remote = STREAM_CFG_REMOTE;
    else if (!strcmp(a, "-packetsize")) { NEEDV(); o->packet_size = atoi(v); }
    else if (!strcmp(a, "-keydir")) { NEEDV(); o->keydir = v; }
    else if (!strcmp(a, "-quitappafter")) o->quit_after = true;
    else if (!strcmp(a, "-stats")) o->stats = true;
    else if (!strcmp(a, "-pin")) { NEEDV(); o->pin = atoi(v); }
    else if (!strcmp(a, "-cpu")) { NEEDV(); o->cpu = atoi(v); }
    else if (!strcmp(a, "-debuginput")) o->debug_input = true;
    else if (!strcmp(a, "-stickdpad")) o->stick_dpad = true;
    else if (!strcmp(a, "-nomenucfg")) o->no_menu_cfg = true;
    else if (!strcmp(a, "-pacing")) o->pacing = true;
    else if (!strcmp(a, "-guard")) o->guard = true;
    else if (!strcmp(a, "-genlock")) o->genlock = true;
    else if (!strcmp(a, "-core")) { NEEDV(); o->core_rbf = v; }
    else { fprintf(stderr, "Opzione sconosciuta: %s\n", a); return -1; }
  }
  if (o->width < 64 || o->height < 64 || o->width > 1920 || o->height > 1088) {
    fprintf(stderr, "Risoluzione non valida: %dx%d\n", o->width, o->height);
    return -1;
  }
  return 0;
}

static int find_app(PSERVER_DATA server, const char *name) {
  PAPP_LIST list = NULL;
  if (gs_applist(server, &list) != GS_OK) {
    fprintf(stderr, "Impossibile leggere l'elenco delle app: %s\n", gs_error);
    return -1;
  }
  for (; list; list = list->next)
    if (!strcasecmp(list->name, name))
      return list->id;
  return -1;
}

/* ---------- sessione ---------- */

int mg_stream_session(PSERVER_DATA server, options *o, int app_id, char *msg, size_t msglen) {
  msg[0] = 0;
  end_reason[0] = 0;
  mg_core_info ci;
  mg_core_get_info(&ci);

  STREAM_CONFIGURATION sc;
  LiInitializeStreamConfiguration(&sc);
  sc.width = o->width;
  sc.height = o->height;
  /* Sunshine codifica al ritmo dei quadri del core (59.94: con Refresh Rate su
   * Auto il core segue lo stream e resta a 59.94), o alla metà con -fps 30 */
  int full = (ci.refresh_x100 + 50) / 100;
  bool half = o->fps > 0 && o->fps <= full / 2 + 1;
  sc.fps = half ? full / 2 : full;
  sc.clientRefreshRateX100 = half ? ci.refresh_x100 / 2 : ci.refresh_x100;
  sc.bitrate = o->bitrate ? o->bitrate : 10000;
  sc.packetSize = o->packet_size;
  sc.streamingRemotely = o->remote;
  sc.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
  sc.supportedVideoFormats = VIDEO_FORMAT_H264;
  sc.colorSpace = COLORSPACE_REC_601;
  sc.colorRange = COLOR_RANGE_LIMITED;
  sc.encryptionFlags = ENCFLG_AUDIO;

  long mbs = (long)((sc.width + 15) / 16) * ((sc.height + 15) / 16);
  if (ci.max_mbs && mbs > (long)ci.max_mbs) {
    snprintf(msg, msglen, "%dx%d e' troppo grande per il core (max %u macroblocchi)", sc.width, sc.height, ci.max_mbs);
    return -1;
  }
  {
    FILE *u = fopen("/proc/uptime", "r");
    double up = 0;
    if (u) { if (fscanf(u, "%lf", &up) != 1) up = 0; fclose(u); }
    printf("[sessione] avvio a uptime %.1f s\n", up);
  }

  /* Se sul PC gira un'altra app, Sunshine "riprenderebbe" quella: va chiusa prima */
  if (server->currentGame != 0 && server->currentGame != app_id) {
    printf("Chiudo l'app in esecuzione sul PC (%d)\n", server->currentGame);
    gs_quit_app(server);
    server->currentGame = 0;
  }

  int pads = o->pads < 1 ? 1 : o->pads > 4 ? 4 : o->pads;
  int ret = gs_start_app(server, &sc, app_id, false, false, (1 << pads) - 1);
  if (ret != GS_OK) {
    snprintf(msg, msglen, "Avvio dell'app fallito (%d): %s", ret, gs_error ? gs_error : "");
    fprintf(stderr, "%s\n", msg);
    return -1;
  }
  printf("Stream %dx%d a %d fps (%.2f), %d kbps, core %s a %.2f Hz\n", sc.width, sc.height, sc.fps,
         sc.clientRefreshRateX100 / 100.0, sc.bitrate, ci.p240 ? "240p" : ci.interlaced ? "interlacciato" : "progressivo",
         ci.refresh_x100 / 100.0);

  mg_core_set_ritmo(o->stats);
  mg_core_set_pacing(o->pacing);
  mg_core_set_guard(o->guard);
  mg_core_set_genlock(o->genlock);
  mg_core_stream_begin();
  while (sem_trywait(&quit_sem) == 0) {} /* richieste vecchie */
  stop_play = false;
  mg_input_set_debug(o->debug_input);
  mg_input_set_stick_dpad(o->stick_dpad);
  if (LiStartConnection(&server->serverInfo, &sc, &conn_cb, &mg_video_callbacks,
                        o->audio ? &mg_audio_callbacks : NULL, NULL, 0, NULL, 0) != 0) {
    snprintf(msg, msglen, "%s", end_reason[0] ? end_reason : "Connessione allo stream fallita");
    fprintf(stderr, "%s\n", msg);
    mg_core_stream_end();
    return -1;
  }
  mg_input_set_mode(MG_INPUT_STREAM);

  sem_wait(&quit_sem);
  printf("Chiusura...\n");
  mg_input_set_mode(MG_INPUT_MENU);
  mg_rumble_stop_all();
  LiStopConnection();
  printf("[fine] stream chiuso\n");
  mg_core_stream_end();
  if (o->stats)
    mg_core_print_counters();
  if (net_trouble) {
    net_trouble = false;
    mg_net_diag("stream interrotto", o->host);
  }
  if (o->quit_after && !exiting) {
    gs_quit_app(server);
    server->currentGame = 0;
    printf("[fine] app chiusa sul PC\n");
  }
  snprintf(msg, msglen, "%s", end_reason);
  return 0;
}

/* ---------- prova senza Sunshine ---------- */

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int do_play(options *o) {
  FILE *f = fopen(o->host, "rb");
  if (!f) {
    perror(o->host);
    return 1;
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *data = malloc((size_t)size);
  if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) {
    fclose(f);
    return 1;
  }
  fclose(f);
  size_t nau = h264ring_split_annexb(data, (size_t)size, NULL, 0);
  struct h264ring_au *au = calloc(nau ? nau : 1, sizeof(*au));
  h264ring_split_annexb(data, (size_t)size, au, nau);
  printf("%s: %zu access unit\n", o->host, nau);

  if (mg_core_open(on_core_gone, on_refused) != 0)
    return 1;
  mg_input_start(o->pads, request_quit);
  mg_input_set_mode(MG_INPUT_STREAM);
  mg_core_stream_begin();
  mg_video_callbacks.setup(VIDEO_FORMAT_H264, 0, 0, 60, NULL, 0);
  mg_core_info ci;
  mg_core_get_info(&ci);
  double period = o->fps > 0 ? 1.0 / o->fps : 100.0 / ci.refresh_x100;
  double next = now_s();
  for (size_t i = 0; i < nau && !stop_play; i++) {
    const uint8_t *p = data + au[i].off;
    bool idr = false;
    for (uint32_t k = 0; k + 3 < au[i].len; k++)
      if (p[k] == 0 && p[k + 1] == 0 && p[k + 2] == 1 && (p[k + 3] & 0x1F) == 5) { idr = true; break; }
    LENTRY e = { .next = NULL, .data = (char *)p, .length = (int)au[i].len, .bufferType = BUFFER_TYPE_PICDATA };
    DECODE_UNIT du = { .frameNumber = (int)i, .frameType = idr ? FRAME_TYPE_IDR : FRAME_TYPE_PFRAME,
                       .fullLength = (int)au[i].len, .bufferList = &e };
    mg_video_callbacks.submitDecodeUnit(&du);
    next += period;
    double wait = next - now_s();
    if (wait > 0) {
      struct timespec ts = { (time_t)wait, (long)((wait - (time_t)wait) * 1e9) };
      nanosleep(&ts, NULL);
    }
  }
  mg_video_callbacks.cleanup();
  mg_core_stream_end();
  mg_core_print_counters();
  mg_input_stop();
  mg_core_close();
  free(au);
  free(data);
  return 0;
}

static int do_stream(PSERVER_DATA server, options *o) {
  int app_id = find_app(server, o->app);
  if (app_id < 0) {
    fprintf(stderr, "App '%s' non trovata (usa 'list' per vederle)\n", o->app);
    return 1;
  }
  if (mg_core_open(on_core_gone, on_refused) != 0)
    return 1;
  mg_input_start(o->pads, request_quit);
  char msg[200];
  int r = mg_stream_session(server, o, app_id, msg, sizeof(msg));
  mg_input_stop();
  mg_core_close();
  return r ? 1 : 0;
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  options o;
  if (parse(argc, argv, &o) != 0 || !strcmp(o.action, "help")) {
    usage();
    return 1;
  }
  mg_core_set_rbf(o.core_rbf);
  if (!o.host) {
    fprintf(stderr, "Serve l'indirizzo IP del PC con Sunshine\n");
    return 1;
  }
  if (o.cpu >= 0) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(o.cpu, &set);
    sched_setaffinity(0, sizeof(set), &set);
  }

  sem_init(&quit_sem, 0, 0);
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  signal(SIGPIPE, SIG_IGN);

  printf("mister_moonlight %s\n", MG_VERSION);
  if (!strcmp(o.action, "play"))
    return do_play(&o);

  char keydir[PATH_MAX];
  if (o.keydir)
    snprintf(keydir, sizeof(keydir), "%s", o.keydir);
  else
    default_keydir(keydir, sizeof(keydir));

  if (!strcmp(o.action, "menu")) {
    if (mg_core_open(on_core_gone, on_refused) != 0)
      return 1;
    int r = mg_menu_run(&o, keydir);
    mg_core_close();
    return r;
  }

  SERVER_DATA server;
  printf("Connessione a %s...\n", o.host);
  int ret = gs_init(&server, o.host, 47989, keydir, 0, true);
  if (ret != GS_OK) {
    fprintf(stderr, "Impossibile contattare Sunshine su %s (%d): %s\n", o.host, ret, gs_error ? gs_error : "");
    return 1;
  }
  if (!strcmp(o.action, "pair")) {
    char pin[5];
    if (o.pin > 0 && o.pin <= 9999)
      snprintf(pin, sizeof(pin), "%04d", o.pin);
    else {
      srandom(getpid() ^ time(NULL));
      snprintf(pin, sizeof(pin), "%04ld", random() % 10000);
    }
    printf("Inserisci questo PIN nella pagina web di Sunshine (scheda PIN): %s\n", pin);
    if (gs_pair(&server, pin) != GS_OK) {
      fprintf(stderr, "Abbinamento fallito: %s\n", gs_error);
      return 1;
    }
    printf("Abbinato.\n");
    return 0;
  }
  if (!strcmp(o.action, "unpair")) {
    if (gs_unpair(&server) != GS_OK) {
      fprintf(stderr, "Errore: %s\n", gs_error);
      return 1;
    }
    printf("Abbinamento rimosso.\n");
    return 0;
  }
  if (!server.paired) {
    fprintf(stderr, "Prima abbina il MiSTer: mister_moonlight pair %s\n", o.host);
    return 1;
  }
  if (!strcmp(o.action, "list")) {
    PAPP_LIST list = NULL;
    if (gs_applist(&server, &list) != GS_OK) {
      fprintf(stderr, "Errore: %s\n", gs_error);
      return 1;
    }
    for (int i = 1; list; list = list->next, i++)
      printf("%d. %s\n", i, list->name);
    return 0;
  }
  if (!strcmp(o.action, "quit"))
    return gs_quit_app(&server) == GS_OK ? 0 : 1;
  if (!strcmp(o.action, "stream"))
    return do_stream(&server, &o);

  usage();
  return 1;
}
