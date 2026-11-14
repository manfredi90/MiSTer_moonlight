#ifndef MG_INPUT_H
#define MG_INPUT_H

#include <stdbool.h>

/* In stream i comandi vanno a Sunshine, nel menu diventano tasti di navigazione */
enum { MG_INPUT_STREAM = 0, MG_INPUT_MENU = 1 };
enum { MG_KEY_NONE = 0, MG_KEY_UP, MG_KEY_DOWN, MG_KEY_LEFT, MG_KEY_RIGHT, MG_KEY_OK, MG_KEY_BACK,
       MG_KEY_DEL,              /* Backspace della tastiera (fuori dai campi di testo vale come BACK) */
       MG_KEY_ENTER,            /* Invio della tastiera, Start del pad: come OK, ma nei campi di testo conferma */
       MG_KEY_CHAR = 0x100 };   /* + un carattere ASCII scritto con la tastiera */

/* Legge pad, tastiera e mouse che il Main passa al core H264 (il core li copia
 * nel suo blocco di controllo: il Main tiene per sé /dev/input). on_quit: la
 * combinazione di uscita o "Program menu" dall'OSD del MiSTer. */
void mg_input_start(int pads, void (*on_quit)(void));
void mg_input_set_mode(int m);
/* Prossimo tasto del menu (MG_KEY_NONE se non arriva niente entro timeout_ms) */
int  mg_input_menu_key(int timeout_ms);
void mg_input_stop(void);
void mg_input_set_debug(bool on);
/* false: toglie le direzioni digitali che il MiSTer ricava dalla levetta sinistra */
void mg_input_set_stick_dpad(bool on);

/* Callback di vibrazione di moonlight-common-c */
void mg_rumble(unsigned short controller, unsigned short low, unsigned short high);
void mg_rumble_stop_all(void);

/* Per i test: come mg_input_start ma senza thread, e un giro del ciclo (ms trascorsi) */
void mg_input_setup(int pads, void (*on_quit)(void));
void mg_input_poll_once(unsigned ms);

#endif
