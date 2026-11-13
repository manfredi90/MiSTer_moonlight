# H264 core for MiSTer Moonlight

The MiSTer H264 core (owenb321/MiSTer_h264_decoder, GPL-3.0-or-later) as extended by
MisterZine Jellyfin (matijaerceg/misterzine-jellyfin, `third_party/MiSTer_h264_decoder` at
b522f4f18b9b6f4688f572c32eea841ce8d99dc2): HPS interface 1.4, Output Mode, and **240p Playback**, which shows a playing stream
on a progressive 240p raster (288p at 50 Hz) at a constant line length, 59.914 Hz (49.993 Hz).

Changes here, all in `h264/emu.sv`:

- the core is named H264 again (`_Other/H264_*.rbf`, loaded by the client);
- the pad's J line is upstream's twelve Xbox-style buttons, and the input region carries
  them as they are (the Jellyfin build had five);
- the OSD's *Send Home to the program* and *Program menu* entries are back;
- `CORE_VERSION` is `mm-<version>`.

The client asks Sunshine for the core's own rate: 59.91 fps at 240p, 59.94 at 480i.

Built by `.github/workflows/core.yml` with Quartus 17.0.2 Lite, several fitter seeds in
parallel, the one with the best timing kept (artifact `H264_core`).
