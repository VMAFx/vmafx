- **One encoder registry feeds the Go and the Python tuners
  ([ADR-3388](docs/adr/3388-central-encoder-registry.md)).** Every encoder's
  quality knob, quality window, absolute range, presets, adapter version and
  capability flags now come from `api/vmafx-encoders.toml`, generated into
  `pkg/encoderregistry` (Go `vmafx-tune`) and the Python `vmaf-tune` package;
  the readable table is [docs/usage/encoder-registry.md](docs/usage/encoder-registry.md).
  The Go `h264_nvenc`/`hevc_nvenc`/`av1_nvenc`, AMF, `libaom-av1` and
  `libsvtav1` adapters now report the same adapter version as their Python
  twins, so cache keys agree between the two tools.
