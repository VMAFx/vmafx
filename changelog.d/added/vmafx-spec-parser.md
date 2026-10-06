- **Model and feature specification strings in the VMAFx API (RC4 WP9, ABI 0.1.10).**
  `vmafx_model_load_spec()` loads the model a string such as
  `version=vmaf_v0.6.1:name=vmaf:disable_clip` or
  `path=model.json` names (keys `version`, `path`,
  `name`, `disable_clip`, `enable_transform` and `<extractor>.<option>=<value>`
  overrides; a backslash escapes the next `:`, `=`, `.` or backslash; an empty
  string is the default model), and `vmafx_context_use_feature_spec()`
  registers the extractor a string such as `psnr` or `cambi=full_ref=true`
  (or upstream FFmpeg's `name=psnr`) names. An item that is not understood
  returns `VMAFX_E_INVALID` naming it, and a string over 4096 bytes or more
  than 64 items `VMAFX_E_RANGE`. The FFmpeg `vmafx` filter and the GStreamer
  `vmafx` element spell their `model` and `feature` options with them, so one
  string means the same thing on every surface. See
  [the model API](docs/api/vmafx/model.md).
