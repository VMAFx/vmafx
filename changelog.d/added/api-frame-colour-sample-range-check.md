- **VMAFx frame colour and the sample range check (VMAFx API 0.1.6).** A
  frame carries its colour in `VmafxFrameDesc.color`, and
  `vmafx_context_set_default_color()` gives the colour of the frames that
  carry none; a model with a `conversion_target` converts every pair from it,
  and a pair of another colour after the first converted one is refused with
  `VMAFX_E_BUSY`. The context option `check_sample_range` of
  `vmafx_context_set_option()` refuses a sample above 2^bpc - 1. The libvmaf
  functions `vmaf_set_input_colorimetry()` and
  `vmaf_set_sample_range_check_enabled()` are compat functions on these, so the
  `vmaf` command line links against the split library again
  ([Frame colour](docs/api/vmafx/index.md#frame-colour),
  [ADR-2094](docs/adr/2094-libvmaf-compat-library-split.md)).
