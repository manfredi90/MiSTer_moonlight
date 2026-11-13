#!/bin/bash
# Ferma MiSTer Moonlight se e' rimasto attivo
killall mister_moonlight 2>/dev/null && echo "MiSTer Moonlight fermato" || echo "MiSTer Moonlight non e' attivo"
