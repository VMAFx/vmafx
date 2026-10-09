## Windows math-constant define on the SYCL command lines (2026-10-09)

- `core/meson.build`: the Windows `-D_USE_MATH_DEFINES` is one list,
  `vmaf_math_constant_args` (empty on other hosts), used for the project
  argument and the header checks. **On sync**: upstream (`4e150067b`) spells
  `add_project_arguments('-D_USE_MATH_DEFINES', ...)` literally; keep the
  fork's list form, which `core/src/meson.build` reuses.
- `core/src/meson.build`: `sycl_common_args` and `sycl_feature_tail_args` add
  the list, because icpx custom targets never see project arguments. A new
  icpx compile line must take one of the two lists
  (`core/test/test_sycl_math_constants_contract.py`).
- `scripts/dev/preflight.sh`: the msvcism stage reads the list form and both
  SYCL lists.
