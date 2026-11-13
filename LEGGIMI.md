# MiSTer Moonlight

Client Moonlight per Sunshine sul MiSTer, con uscita sul core H264 (decoder H.264 in FPGA).

## Installazione

1. Copia sulla SD (in `/media/fat`) le cartelle `Scripts` e `MiSTer_moonlight` dello zip,
   e il core H264 in `_Other` (`H264_*.rbf`).
2. Lancia una volta **Scripts → mister_moonlight**: crea `MiSTer_moonlight/mister_moonlight.conf`.
   Mettici l'indirizzo del PC con Sunshine (`HOST=...`) e rilancia.
3. Se in `/media/fat/moonlight-groovy/keys` c'è già un abbinamento, viene riusato; se no il
   menu mostra un PIN da inserire nella pagina web di Sunshine (scheda PIN).

In Sunshine tieni **CAVLC disattivato**: il core decodifica CABAC; uno stream CAVLC viene
riscritto sull'ARM, che regge solo bitrate bassi.

## Comandi

- Menu: croce o frecce; A, Invio o Start conferma; B o Esc indietro (dalla lista: esce).
- Tornare al menu durante lo stream: Select+Start+LB+RB, Ctrl+Alt+Shift+Q, oppure
  *Program menu* nell'OSD del MiSTer.
- Tasto Guide: Select+Start, oppure *Send Home to the program* nell'OSD.
- Modalità mouse: Start tenuto da solo 1 s (una breve vibrazione lo conferma). Levetta sinistra
  o croce = puntatore, levetta destra = rotellina, A/B/X = tasti sinistro/destro/centrale.

## Impostazioni

Nel menu: risoluzione chiesta a Sunshine (640x480 predefinita), bitrate (automatico =
10 Mbps), FPS (60 come il core oppure 30), audio, chiusura dell'app all'uscita. Le scelte
restano in `MiSTer_moonlight/menu.cfg`.

Sunshine riceve la frequenza dei quadri del core: 59,94 a 480i (con *Refresh Rate* su Auto
il core resta a 59,94), 50 se il core è a 50 Hz. Così nessun frame viene saltato o ripetuto.

## 240p

Serve il core di questo repository (cartella `core/`, artefatto `H264_core` delle GitHub
Actions): nell'OSD del core scegli *Output Mode* 480i60 e *240p Playback* On. Il menu resta a
480i, lo stream passa a 240p a 59,914 Hz (righe tutte uguali) e Sunshine riceve 59,91 fps.
Come risoluzione conviene 640x240 o 320x240.

Il log dell'ultima sessione è in `MiSTer_moonlight/ultimo_stream.log`.
