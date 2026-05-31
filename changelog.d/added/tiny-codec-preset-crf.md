- **`--tiny-codec` / `--tiny-preset` / `--tiny-crf` CLI flags** populate
  the codec one-hot block of codec-aware tiny models (today
  `fr_regressor_v2`) so the model receives the real encoder context
  instead of the ADR-0518 "unknown" pre-seed. Encoder names are
  validated against the model sidecar's `encoder_vocab`; unknown
  names hard-fail at attach time so typos are caught. Common ffprobe
  aliases (`h264`, `hevc`, `av1`, `vp9`, `vvc`) are accepted. Public
  API: `vmaf_dnn_set_codec_context()` in `libvmaf/dnn.h`. See
  [ADR-0522](../docs/adr/0522-tiny-codec-preset-crf-cli-flags.md).
