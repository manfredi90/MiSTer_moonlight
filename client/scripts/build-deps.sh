#!/bin/bash
# Cross-compila le dipendenze statiche per l'ARM del MiSTer (Cortex-A9, NEON, hard-float).
# Uso: build-deps.sh <dir sorgenti> <prefix>
# Se nella cartella dei sorgenti mancano, li scarica (da GitHub) e ne controlla lo SHA-256.
set -euo pipefail
mkdir -p "$1"
SRC=$(realpath "$1"); PREFIX=$(realpath -m "$2")
fetch() { # file url sha256
  [ -f "$SRC/$1" ] || curl -fsSL -o "$SRC/$1" "$2"
  echo "$3  $SRC/$1" | sha256sum -c --quiet
}
fetch openssl-3.0.15.tar.gz https://github.com/openssl/openssl/releases/download/openssl-3.0.15/openssl-3.0.15.tar.gz \
  23c666d0edf20f14249b3d8f0368acaee9ab585b09e1de82107c66e1f3ec9533
fetch expat-2.6.3.tar.xz https://github.com/libexpat/libexpat/releases/download/R_2_6_3/expat-2.6.3.tar.xz \
  274db254a6979bde5aad404763a704956940e465843f2a9bd9ed7af22e2c0efc
fetch curl-8.10.1.tar.xz https://github.com/curl/curl/releases/download/curl-8_10_1/curl-8.10.1.tar.xz \
  73a4b0e99596a09fa5924a4fb7e4b995a85fda0d18a2c02ab9cf134bebce04ee
if [ ! -d "$SRC/opus" ]; then
  git clone -q https://github.com/xiph/opus "$SRC/opus"
  git -C "$SRC/opus" checkout -q ddbe48383984d56acd9e1ab6a090c54ca6b735a6 # v1.5.2
fi
HOST=arm-linux-gnueabihf
export CC=$HOST-gcc CXX=$HOST-g++ AR=$HOST-ar RANLIB=$HOST-ranlib STRIP=$HOST-strip
ARCHFLAGS="-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard"
export CFLAGS="-O2 $ARCHFLAGS -fPIC" CXXFLAGS="-O2 $ARCHFLAGS -fPIC"
export PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig PKG_CONFIG_PATH=
J=$(nproc)
mkdir -p "$PREFIX" /tmp/mgbuild

if [ ! -f "$PREFIX/lib/libcrypto.a" ]; then
  rm -rf /tmp/mgbuild/openssl && mkdir -p /tmp/mgbuild/openssl
  tar xf "$SRC"/openssl-*.tar.gz -C /tmp/mgbuild/openssl --strip-components=1
  cd /tmp/mgbuild/openssl
  ./Configure linux-armv4 no-shared no-tests no-engine no-module \
     --prefix="$PREFIX" --libdir=lib $ARCHFLAGS
  make -j$J build_libs >/dev/null && make install_dev >/dev/null
fi

if [ ! -f "$PREFIX/lib/libexpat.a" ]; then
  rm -rf /tmp/mgbuild/expat && mkdir -p /tmp/mgbuild/expat
  tar xf "$SRC"/expat-*.tar.xz -C /tmp/mgbuild/expat --strip-components=1
  cd /tmp/mgbuild/expat
  ./configure --host=$HOST --prefix="$PREFIX" --disable-shared --enable-static \
     --without-docbook --without-examples --without-tests >/dev/null
  make -j$J >/dev/null && make install >/dev/null
fi

if [ ! -f "$PREFIX/lib/libcurl.a" ]; then
  rm -rf /tmp/mgbuild/curl && mkdir -p /tmp/mgbuild/curl
  tar xf "$SRC"/curl-*.tar.xz -C /tmp/mgbuild/curl --strip-components=1
  cd /tmp/mgbuild/curl
  ./configure --host=$HOST --prefix="$PREFIX" --disable-shared --enable-static \
     --with-openssl="$PREFIX" --without-zlib --without-brotli --without-zstd \
     --without-libpsl --without-libidn2 --without-nghttp2 --without-libssh2 \
     --disable-ldap --disable-rtsp --disable-dict --disable-telnet --disable-tftp \
     --disable-pop3 --disable-imap --disable-smtp --disable-gopher --disable-mqtt \
     --disable-manual --disable-docs --enable-ipv6 --with-ca-bundle=/etc/ssl/certs/ca-certificates.crt >/dev/null
  make -j$J -C lib >/dev/null && make -C lib install >/dev/null && make -C include install >/dev/null
  install -m644 libcurl.pc "$PREFIX/lib/pkgconfig/"
fi

if [ ! -f "$PREFIX/lib/libopus.a" ]; then
  rm -rf /tmp/mgbuild/opus && mkdir -p /tmp/mgbuild/opus && cd /tmp/mgbuild/opus
  cmake -G Ninja "$SRC/opus" -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=arm \
     -DCMAKE_C_COMPILER=$CC -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
     -DOPUS_BUILD_SHARED_LIBRARY=OFF -DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_PROGRAMS=OFF >/dev/null
  ninja >/dev/null && ninja install >/dev/null
fi

echo "Dipendenze pronte in $PREFIX"
