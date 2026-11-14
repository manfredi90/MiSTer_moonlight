#ifndef MG_CORE_H
#define MG_CORE_H

#include <Limelight.h>
#include <stdbool.h>
#include <stdint.h>

struct h264ring;

/* Carica il core H264 (se non è già quello attivo), si collega al suo ring e
 * apre una sessione che resta aperta per tutta l'esecuzione: serve anche al
 * menu, perché il core mostra l'overlay solo durante una sessione. */
int  mg_core_open(void (*on_gone)(void), void (*on_refused)(const char *why));
/* Per i test: si collega a un ring già pronto (core finto in memoria) */
int  mg_core_attach(struct h264ring *r, void (*on_gone)(void), void (*on_refused)(const char *why));
void mg_core_close(void);
/* Per i test: un framebuffer in memoria al posto di /dev/fb0 */
void mg_core_use_fb(void *virt, uint32_t phys, uint32_t len);
struct h264ring *mg_core_ring(void);

/* Rilegge lo stato del core: battito (core scaricato?), OSD aperto, errori di
 * decodifica (chiede un IDR), sessione persa (la riapre). Ogni ~50 ms. */
void mg_core_poll(void);
/* Diagnostica del ritmo delle immagini (-stats): ogni ~2 ms durante lo stream, dal thread
 * dell'input; stampa le immagini saltate/ripetute e ogni 10 s un riepilogo */
void mg_core_set_ritmo(bool on);
/* Il passo: un'immagine al core solo dopo che la precedente è stata presa (predefinito: sì) */
void mg_core_set_pacing(bool on);
void mg_core_tick(void);
bool mg_core_osd_open(void);

/* Frequenza dei quadri del core e fps da chiedere a Sunshine (x100) */
typedef struct {
  int refresh_x100;   /* 5994, 6000, 5000; 5991 / 4999 a 240p / 288p */
  bool interlaced;    /* 480i/576i sull'uscita analogica */
  bool p240;          /* 240p/288p durante lo stream (core di core/, 240p Playback) */
  int raster_h;       /* 480 o 576 */
  int raster_w;       /* 720 (core originale) o 640 (core di core/) */
  unsigned max_mbs, mb_rate;
} mg_core_info;
void mg_core_get_info(mg_core_info *ci);

/* Inizio e fine di uno stream: svuota il ring e azzera il decoder (schermo
 * nero fino al primo IDR), nasconde/mostra l'overlay del menu. */
void mg_core_stream_begin(void);
void mg_core_stream_end(void);

/* Overlay: copia il canvas (0xRRGGBB, w x h) ingrandito x2 e lo mostra */
int  mg_core_overlay_present(const uint32_t *canvas, int w, int h);
void mg_core_overlay_hide(void);

extern DECODER_RENDERER_CALLBACKS mg_video_callbacks;
extern AUDIO_RENDERER_CALLBACKS mg_audio_callbacks;

typedef struct {
  unsigned long au, au_bytes, au_dropped, idr_asked, errors_seen, sessions, refusals;
  unsigned long aud, aud_dropped, aud_early, aud_plc, au_rewritten;
} mg_core_counters;
void mg_core_get_counters(mg_core_counters *c);
void mg_core_print_counters(void);

#endif
