## Model conversion target: assumed colour for untagged input (2026-10-19)

- Port of Netflix/vmaf `272c596770` (`assumed_color_if_untagged`). Upstream
  sets `VmafPicture::color` of both pictures in `vmaf_read_pictures()`; the
  fork has no colour on the picture (ADR-1822) and resolves the assumed colour
  into local copies inside `vmaf_conversion_state_convert()`
  (`core/src/conversion_context.c`). **On sync**: keep the fork's side of the
  hunks in `libvmaf.c` and `conversion_context.c`; take upstream's policy
  logic into `vmaf_conversion_policy_assume_color()`, which works on colours.
- The model parser change is in both `read_json_model.cpp` and
  `read_json_model.c` through `parse_color_object()`.
