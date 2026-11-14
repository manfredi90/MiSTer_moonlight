#ifndef MG_OPTIONS_H
#define MG_OPTIONS_H

#include <client.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
  const char *action;
  char *host;
  const char *app;
  const char *keydir;
  int width, height; /* risoluzione chiesta a Sunshine */
  int bitrate;       /* kbps, 0 = automatico */
  int fps;           /* 0 = come i quadri del core, 30 = metà */
  int pads;
  int remote;
  int packet_size;
  bool audio;
  bool quit_after;
  bool stats;
  bool debug_input;
  bool stick_dpad;
  int cpu;
  int pin;
  bool no_menu_cfg;
  bool pacing;
} options;

/* Sessione di streaming col core già collegato. Ritorna 0 se lo stream è
 * partito (e poi è finito), -1 se non è partito; 'msg' riceve il motivo. */
int mg_stream_session(PSERVER_DATA server, options *o, int app_id, char *msg, size_t msglen);
/* Chiede la fine della sessione in corso (come la combinazione di tasti) */
void mg_request_quit(void);
/* true se il programma deve uscire (SIGTERM/SIGINT, core scaricato) */
bool mg_exiting(void);

/* menu.c */
int mg_menu_run(options *o, const char *keydir);

/* risoluzioni proposte nel menu */
typedef struct { int w, h; const char *desc; } mg_res;
int mg_res_count(void);
const mg_res *mg_res_at(int i);
int mg_res_find(int w, int h); /* indice, -1 se non c'è */

#endif
