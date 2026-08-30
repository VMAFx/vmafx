- **FFmpeg-patch series for vmaf-tune integration (ADR-0312, patches 0007–0009).**
  Adds three patches against FFmpeg n8.1 under `ffmpeg-patches/`:
  (1) `0007-libvmaf-tune-qpfile-unified.patch` — unified `-qpfile <path>`
  AVOption on `libx264`, `libsvtav1`, and `libaom-av1` with a shared parser
  at `libavcodec/qpfile_parser.{c,h}` for the format emitted by
  `tools/vmaf-tune/src/vmaftune/saliency.py`. libx264 is fully wired
  (forwards to x264's native per-MB qpfile reader); SVT-AV1 / libaom parse
  + log (full ROI bridges deferred per ADR-0312 Alternatives).
  (2) `0008-add-libvmaf_tune-filter.patch` — new `libvmaf_tune` 2-input
  filter that emits a `recommended_crf=…` log line at uninit; scaffold
  with linear CRF↔VMAF interpolation (full Optuna TPE stays in
  `tools/vmaf-tune/src/vmaftune/recommend.py`). (3)
  `0009-pass-autotune-cli-glue.patch` — `-pass-autotune` advisory flag in
  `fftools/ffmpeg_opt.c` pointing at `docs/usage/vmaf-tune-ffmpeg.md`.
  All 9 patches series-replay cleanly against pristine `n8.1`. New user
  doc at [`docs/usage/vmaf-tune-ffmpeg.md`](docs/usage/vmaf-tune-ffmpeg.md);
  research digest at
  [`docs/research/0084-ffmpeg-patch-vmaf-tune-integration-survey.md`](docs/research/0084-ffmpeg-patch-vmaf-tune-integration-survey.md).
