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

Nel menu: bitrate (automatico = 10 Mbps), audio, chiusura dell'app all'uscita. Le scelte
restano in `MiSTer_moonlight/menu.cfg`. Lo stream è sempre 640x480 a 60 immagini al secondo
(quanto il core mostra 1:1); da riga di comando `-width`, `-height` e `-fps` restano per le prove.

Sul PC conviene una modalità video a **60,000 Hz esatti**: la 640x480 "60 Hz" standard è in realtà
59,94 Hz, e lo scarto fa saltare o ripetere un'immagine a raffiche ogni ~20 secondi. Va bene una
1280x720 a 60 Hz (74,25 MHz, già esatta) oppure una 640x480 personalizzata (pannello NVIDIA,
tempistica manuale): orizzontale 640/16/96/800, verticale 480/10/2/525, clock pixel 25,2 MHz.
Con `-stats` il log dice ogni 10 s a quanti fps va il PC.

Sunshine riceve la frequenza dei quadri del core: 59,94 a 480i (con *Refresh Rate* su Auto
il core resta a 59,94), 50 se il core è a 50 Hz. Così nessun frame viene saltato o ripetuto.

## Il core di questo repository

Il core in `core/` (artefatto `H264_core` delle GitHub Actions) ha il quadro largo **640 pixel**
e va a **60,000 Hz esatti** in tutti i modi, con righe tutte uguali: uno stream 640x480 arriva 1:1,
uno 320x240 esattamente x2, senza ridimensionamenti in orizzontale. Sunshine riceve 60 fps.

Nell'OSD del core: *Output Mode* 15kHz (480i) o 31kHz (480p); a 15kHz, *15kHz Mode* 240p fa
passare lo stream a 240p (il menu resta a 480i; lo stream 640x480 a righe alterne).
Niente 50 Hz.

Col core originale (H264 v1.1.0) il client funziona lo stesso: Sunshine riceve 59,94 fps e il
menu viene allargato ai 720 pixel del suo quadro.
