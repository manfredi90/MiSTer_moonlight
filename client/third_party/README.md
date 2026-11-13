Vendored sources:

- `moonlight-common-c/`, `libgamestream/`: from moonlight-embedded (GPL-3.0), as used by
  moonlight-groovy; `libgamestream/http.c` and `client.c` carry fixes for Sunshine's
  single-threaded HTTPS server (no connection reuse, timeouts, the OS error in `gs_error`).
- `h264ring/`: owenb321/MiSTer_h264_decoder `hps/` (MIT), tag v1.1.0
  (eac10641c5af0b808d332bb61344ee2dfdba4271): `include/` and `lib/` unchanged.
