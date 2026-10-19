---
paths:
  - core/src/conversion_context.c
  - core/src/conversion_context.h
  - core/src/conversion_policy.c
  - core/src/conversion_policy.h
  - core/src/read_json_model.c
  - core/src/read_json_model.cpp
invariant: Convert to model's conversion_target on host; input colour lives on context.
---
<!-- markdownlint-disable MD013 -->
# Model conversion target and input colorimetry

## `conversion_target` (Netflix/vmaf `1ddf81607`, `a6c0ba6d5`; ADR-2093)

- `vmaf_read_pictures()` calls `vmaf_conversion_state_convert()` first, before
  validation and before any device translation. Zimg reads host planes, so
  device picture returns `-ENOTSUP`; do not move call behind CUDA
  translation. failed conversion releases both pictures (ADR-1431); upstream
  leaves them to caller.
- source colour of each input is `VmafConversionState::ref_color` /
  `dist_color`, set by `vmaf_set_input_colorimetry()`. `VmafPicture` has no
  colour (ADR-1822): never read `pic->color`, never add member. policy
  functions take colour as argument.
- All models of run declare same target or none
  (`vmaf_conversion_state_register_model()`); model without target makes
  whole step pass-through.
- `conversion_target` is parsed in `read_json_model.cpp` (built) and
  `read_json_model.c` (fuzz harness). Change both together; block needs all
  four colour attributes and rejects `unknown` values.
- Guards: `test_conversion_policy`, `test_model`, `test_read_pictures_convert`
  (conversions need `-Denable_zimg=true`).
