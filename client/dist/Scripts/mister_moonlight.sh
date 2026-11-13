#!/bin/bash
# MiSTer Moonlight: carica il core H264 e apre il menu di Moonlight (app di Sunshine,
# impostazioni, abbinamento). Durante lo stream Select+Start+LB+RB (o Ctrl+Alt+Shift+Q)
# torna al menu; B nel menu esce e torna al menu del MiSTer.
D=/media/fat/MiSTer_moonlight
BIN=$D/mister_moonlight
[ -x "$BIN" ] || chmod +x "$BIN" 2>/dev/null
[ -f $D/mister_moonlight.conf ] || cp $D/mister_moonlight.conf.esempio $D/mister_moonlight.conf
eval "$(tr -d '\r' < $D/mister_moonlight.conf)"
if [ -z "$HOST" ] || [ "$HOST" = 192.168.1.100 ]; then
  echo "Imposta HOST (l'indirizzo del PC con Sunshine) in $D/mister_moonlight.conf"
  exit 1
fi
if ! ls /media/fat/_Other/H264_*.rbf >/dev/null 2>&1; then
  echo "Core H264 non trovato: copia _Other/H264_*.rbf sulla SD"
  exit 1
fi
if pidof mister_moonlight >/dev/null; then
  echo "MiSTer Moonlight e' gia' in esecuzione"
  exit 1
fi
# chiavi di abbinamento di moonlight-groovy, se ci sono: niente nuovo PIN
if [ ! -d $D/keys ] && [ -d /media/fat/moonlight-groovy/keys ]; then
  cp -r /media/fat/moonlight-groovy/keys $D/keys
fi
# Il risparmio energetico del Wi-Fi peggiora le pause di rete: lo disattiviamo
for W in /sys/class/net/wlan*; do
  [ -e "$W" ] || continue
  N=$(basename $W)
  iw dev $N set power_save off 2>/dev/null || iwconfig $N power off 2>/dev/null
done
LOG=$D/ultimo_stream.log
# Durante lo streaming il log sta in RAM (scrivere sulla SD puo' bloccare); alla fine va sulla SD
TMPLOG=/tmp/mister_moonlight.log
NETINFO="avvio script: uptime $(cut -d' ' -f1 /proc/uptime) s; rete: $(ip route 2>/dev/null | head -1)"
echo "Carico il core H264 e apro Moonlight ($HOST)... log in $LOG"
# Staccato dallo script: sopravvive al caricamento del core. Alla fine torna al menu
# del MiSTer, ma solo se e' ancora attivo il core H264.
setsid nohup bash -c "
  echo \"[script] $NETINFO\"
  ( while sleep 5; do cp $TMPLOG $LOG; done ) &
  COPIER=\$!
  if [ \"$MENU\" = 0 ]; then
    $BIN stream $HOST -app \"$APP\" $EXTRA
  else
    $BIN menu $HOST $EXTRA
  fi
  echo \"[script] uscita: \$?\"
  kill \$COPIER
  cp $TMPLOG $LOG
  [ \"\$(cat /tmp/CORENAME 2>/dev/null)\" = H264 ] && echo 'load_core /media/fat/menu.rbf' > /dev/MiSTer_cmd
" > $TMPLOG 2>&1 < /dev/null &
sleep 1
