- **A native `vmafx` GStreamer element on the VMAFx API (RC4, #2236).** The
  element in `gstreamer/` (a standalone Meson project) scores a distorted video
  against a reference with two sink pads, `reference` and `distorted`, and
  passes the distorted frames on. It takes system memory without a copy and
  CUDA memory without a download (device pointers imported on the producer's
  stream, with acquire and release fences); its properties are generated from
  the option table the FFmpeg `vmafx` filter uses; it reports `n_stats` windows
  from the library's window clock, per-frame scores, the provenance record and
  an end-of-stream summary as element messages, and writes the CLI's report. On
  the golden pair, both checkerboards and 20 frames of the 4K clip its report
  equals the `vmaf` CLI's bit for bit on the CPU and on CUDA
  (`gstreamer/test/run.sh`). See `docs/usage/gstreamer.md`.
