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
- `resolution.display` (Netflix/vmaf `ca2a2a111d`): 1080 or 2160 only;
  `vmaf_conversion_policy_scale()` sets the target size (short side = display,
  long side rounded to even, bicubic up / Lanczos down, -EINVAL above 4:1).
- Different input sizes (Netflix/vmaf `cf9a6ec51b`): the policy fills a
  separate `dist_target` with the reference's scaled size and its own filter;
  orientation must match and the scaled long sides may differ by 2 pixels
  (`SCALE_ASPECT_TOLERANCE`). `validate_pic_size()` in `libvmaf.c` runs after
  conversion and keeps the same-size rule for runs without a display. `picture_matches()` compares the pinned size;
  `register_model()` treats a different display as a different target.
- `assumed_color_if_untagged` (Netflix/vmaf `272c596770`): upstream writes
  the assumed colour into both `VmafPicture::color`; the fork resolves it into
  local copies of the state colours (`resolve_input_colors()` in
  `conversion_context.c`) and never into a picture or the state. Only a pair
  with both colours all-unknown takes it (`vmaf_conversion_policy_assume_color()`);
  one tagged input against an untagged one is `-EINVAL`, a partly tagged pair
  falls through to the policy's own rejection. `same_conversion()` treats a
  different assumed colour as a different target. Parsed through the shared
  `parse_color_object()` in both readers.
- Guards: `test_conversion_policy`, `test_model`, `test_read_pictures_convert`
  (conversions need `-Denable_zimg=true`).
