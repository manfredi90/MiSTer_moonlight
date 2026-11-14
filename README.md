# MiSTer Moonlight

A [Moonlight](https://moonlight-stream.org) client for [Sunshine](https://github.com/LizardByte/Sunshine)
that runs on the MiSTer FPGA's ARM and plays the stream through the
[H264 core](https://github.com/owenb321/MiSTer_h264_decoder), which decodes H.264 in the FPGA.
It is aimed at CRTs. With the core in `core/` the picture is 640 square pixels wide and every
mode (480i, 480p, 240p) runs at exactly 60.000 Hz; the stock H264 core (480i at 59.94 Hz) works too.

- On-screen menu (drawn in the core's overlay plane): Sunshine's apps, bitrate, audio, pairing
  with an on-screen PIN. The stream is always 640x480 at 60 fps.
- Pads, keyboard and mouse through the core (Main keeps `/dev/input` while a core runs),
  rumble on the MiSTer's pads, mouse mode on the pad.
- Sunshine is asked for the core's own frame rate (59.94 fps on a 59.94 Hz raster), so the
  core never drops or repeats a frame.

## Layout

| Folder | What |
| --- | --- |
| `client/` | the program (C, cross-compiled for the Cortex-A9, static) |
| `core/` | a build of the H264 core with 240p output for game streams |

## Install

Copy the release zip's folders to the root of the SD card (`/media/fat`), together with the
H264 core (`_Other/H264_*.rbf`). Set `HOST` (the PC running Sunshine) in
`MiSTer_moonlight/mister_moonlight.conf` (created from `.conf.esempio` on the first run), then
run **Scripts → mister_moonlight**.

In Sunshine, leave **CAVLC** off: the core decodes CABAC; a CAVLC stream is rewritten on the
ARM, which only keeps up at low bitrates.

## Controls

| | |
| --- | --- |
| Menu | d-pad or arrows, A / Enter / Start confirm, B / Esc back |
| Back to the menu | Select+Start+LB+RB, Ctrl+Alt+Shift+Q, or *Program menu* in the MiSTer OSD |
| Guide | Select+Start, or *Send Home to the program* in the MiSTer OSD |
| Mouse mode | hold Start for 1 s: left stick or d-pad = pointer, right stick = wheel, A/B/X = buttons |

## Build

On Linux (Ubuntu 24.04: `gcc-arm-linux-gnueabihf cmake ninja-build pkg-config`, plus
`qemu-user` and `ffmpeg` for the test):

    client/scripts/build.sh

It builds the static dependencies (OpenSSL, curl, expat, Opus), the program, runs the test
against a fake core in memory, and writes the SD-card zip.

## Credits and licences

- Program: GPL-3.0-or-later (`LICENSE`).
- `client/third_party/moonlight-common-c`, `client/third_party/libgamestream`:
  [moonlight-stream](https://github.com/moonlight-stream), GPL-3.0.
- `client/third_party/h264ring`: the H264 core's HPS library, MIT
  (owenb321/MiSTer_h264_decoder v1.1.0, `hps/`). The session, error and input handling follow
  that project's own Moonlight client (`clients/moonlight/src/mister`).
