#!/bin/bash
# Compila MiSTer Moonlight per l'ARM del MiSTer, esegue la prova col core finto
# (con qemu-arm, se c'è) e prepara lo zip da copiare sulla SD.
# Uso: scripts/build.sh [cartella di lavoro]   (predefinita: ./work)
set -euo pipefail
CLIENT=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(realpath -m "${1:-$CLIENT/work}")
DEPS=$WORK/deps
bash "$CLIENT/scripts/build-deps.sh" "$WORK/src" "$DEPS"
export PKG_CONFIG_LIBDIR=$DEPS/lib/pkgconfig PKG_CONFIG_PATH=
cmake -S "$CLIENT" -B "$WORK/build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$CLIENT/cmake/arm-mister.cmake" \
  -DDEPS_PREFIX="$DEPS" -DMG_TESTS=ON >/dev/null
ninja -C "$WORK/build"

if command -v qemu-arm >/dev/null && command -v ffmpeg >/dev/null; then
  CLIP=$WORK/test_640x480.h264
  [ -f "$CLIP" ] || ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=640x480:rate=60 -t 2 -c:v libx264 \
    -profile:v high -bf 0 -g 60 -pix_fmt yuv420p -x264-params slices=4 -b:v 4M "$CLIP"
  qemu-arm "$WORK/build/test_core" "$CLIP"
else
  echo "qemu-arm o ffmpeg mancanti: salto la prova col core finto"
fi

VER=$(sed -n 's/^#define MG_VERSION "\(.*\)"/\1/p' "$CLIENT/src/main.c")
PKG=$WORK/pkg
rm -rf "$PKG" && mkdir -p "$PKG/Scripts" "$PKG/MiSTer_moonlight"
cp "$CLIENT"/dist/Scripts/*.sh "$PKG/Scripts/"
cp "$CLIENT"/dist/MiSTer_moonlight/* "$PKG/MiSTer_moonlight/"
cp "$WORK/build/mister_moonlight" "$PKG/MiSTer_moonlight/"
cp "$CLIENT/../LEGGIMI.md" "$PKG/MiSTer_moonlight/" 2>/dev/null || true
(cd "$PKG" && rm -f "$WORK/MiSTer_moonlight-$VER.zip" && zip -qr "$WORK/MiSTer_moonlight-$VER.zip" .)
echo "Pronto: $WORK/MiSTer_moonlight-$VER.zip"
