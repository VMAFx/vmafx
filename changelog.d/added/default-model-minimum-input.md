- **Minimum input size of the default model `vmaf_v1.0.16_3d0h` documented and tested.**
  The model reads CAMBI (width or height at least 216) and `speed_chroma` (chroma width and
  height at least 80), so the smallest 4:2:0 inputs it scores are 216x160 and 160x216. The
  [CLI usage page](docs/usage/cli.md#minimum-input-size) gives the refusal messages and two
  caller recipes (scale both inputs, or pick `vmaf_v0.6.1`); `test_default_model_minimum_input`
  holds the library's `-EINVAL` refusal and the accepted sizes.
