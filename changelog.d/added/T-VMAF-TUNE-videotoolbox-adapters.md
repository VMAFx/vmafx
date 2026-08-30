- `vmaf-tune` Apple VideoToolbox codec adapters (ADR-0283). Adds
  `H264VideoToolboxAdapter` + `HEVCVideoToolboxAdapter` under
  `tools/vmaf-tune/src/vmaftune/codec_adapters/`, sharing a single
  `_videotoolbox_common.py` for the `-q:v` quality knob (0..100,
  higher = better) and the nine-name preset → `-realtime` boolean
  mapping. AV1 hardware encoding intentionally omitted (unsupported
  on Apple Silicon as of 2026). Registry entries `h264_videotoolbox`
  + `hevc_videotoolbox`. Tests mock `subprocess.run` so the suite
  runs on Linux CI without a macOS runner. The originally-coupled
  16-slot codec-vocab schema expansion is deferred to a follow-up PR
  awaiting a fresh `fr_regressor_v2` production retrain.
