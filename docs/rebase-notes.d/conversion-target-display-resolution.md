## Netflix/vmaf ca2a2a111d: scale same-size inputs to a model's display resolution (2026-10-19)

- `core/src/conversion_policy.c`: `vmaf_conversion_policy_scale()` is
  upstream's scaling arithmetic; `picture_matches()` keeps the fork's
  `(pic, pic_color, target)` signature (ADR-1822) and compares the pinned size.
  **On sync**: take upstream's arithmetic, keep the colour argument.
- `core/src/conversion_context.{c,h}`: the state carries `display_resolution`;
  upstream keeps it in the convert struct of `libvmaf.c`. **On sync**: port
  changes to that struct here, not into `core/src/libvmaf.c`.
- `core/src/read_json_model.cpp` and `read_json_model.c` both parse
  `resolution`. **On sync**: change both together.
