cmake_minimum_required(VERSION 3.13)
project(mister_moonlight LANGUAGES C)
set(CMAKE_C_STANDARD 11)

# Cartella con le dipendenze statiche (vedi scripts/build-deps.sh)
set(DEPS_PREFIX "" CACHE PATH "Prefisso delle dipendenze cross-compilate")
if(DEPS_PREFIX)
  list(APPEND CMAKE_FIND_ROOT_PATH ${DEPS_PREFIX})
  set(OPENSSL_ROOT_DIR ${DEPS_PREFIX})
  set(OPENSSL_USE_STATIC_LIBS TRUE)
endif()
option(MG_STATIC "Binario statico" ON)

set(BUILD_SHARED_LIBS OFF)
find_package(OpenSSL REQUIRED)
add_subdirectory(third_party/moonlight-common-c EXCLUDE_FROM_ALL)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET libcurl expat opus)

add_library(gamestream STATIC
  third_party/libgamestream/client.c
  third_party/libgamestream/http.c
  third_party/libgamestream/mkcert.c
  third_party/libgamestream/xml.c)
target_include_directories(gamestream PUBLIC third_party/libgamestream PRIVATE compat)
target_link_libraries(gamestream PUBLIC moonlight-common-c PkgConfig::DEPS OpenSSL::SSL OpenSSL::Crypto)
target_compile_options(gamestream PRIVATE -w)

# Libreria del core H264 (MIT, owenb321/MiSTer_h264_decoder hps/)
add_library(h264ring STATIC
  third_party/h264ring/lib/h264ring.c
  third_party/h264ring/lib/h264ring_linux.c
  third_party/h264ring/lib/h264ring_dump.c
  third_party/h264ring/lib/h264ring_au.c
  third_party/h264ring/lib/h264ring_cavlc.c)
target_include_directories(h264ring PUBLIC third_party/h264ring/include PRIVATE third_party/h264ring/lib)
target_compile_definitions(h264ring PRIVATE _DEFAULT_SOURCE)
set_source_files_properties(third_party/h264ring/lib/h264ring_cavlc.c PROPERTIES COMPILE_OPTIONS "-O3")

add_executable(mister_moonlight
  src/main.c src/core.c src/input.c src/menu.c src/netdiag.c)
target_compile_options(mister_moonlight PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
target_link_libraries(mister_moonlight PRIVATE gamestream h264ring moonlight-common-c PkgConfig::DEPS m
  OpenSSL::SSL OpenSSL::Crypto pthread dl m)
# libopus.a ha bisogno di libm: va dopo tutte le librerie statiche
target_link_options(mister_moonlight PRIVATE LINKER:--start-group)
if(MG_STATIC)
  target_link_options(mister_moonlight PRIVATE -static -s)
endif()

# Prova sul PC: il programma col core finto in memoria (tests/)
option(MG_TESTS "Programma di prova col core finto" OFF)
if(MG_TESTS)
  add_executable(test_core tests/test_core.c src/core.c src/input.c)
  # le funzioni di moonlight-common-c sono finte, nel test: solo le intestazioni
  target_include_directories(test_core PRIVATE src third_party/moonlight-common-c/src)
  target_compile_options(test_core PRIVATE -Wall -Wextra -Wno-unused-parameter -O1 -g)
  target_link_libraries(test_core PRIVATE h264ring PkgConfig::DEPS pthread m)
  target_link_options(test_core PRIVATE -static LINKER:--start-group)
endif()
