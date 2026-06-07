- `ffmpeg-patches/0002`: expand `vmaf_pre` filter `parse_device()` to cover all
  twelve `VmafDnnDevice` values (`openvino-npu`, `openvino-cpu`, `openvino-gpu`,
  `coreml`, `coreml-ane`, `coreml-gpu`, `coreml-cpu` were silently returning
  `AVERROR(EINVAL)`). Update `device=` option help text to list all twelve
  strings. Fix copyright line (drop "and Claude (Anthropic)"). Fix misleading
  struct comment in patch 0014 (`gpumask bit 1` → `gpumask bit 0 (value 1)`).
  Implements ADR-0482.
