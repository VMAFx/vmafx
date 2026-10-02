---
paths:
  - core/src/meson.build
  - core/src/feature/ciede.c
  - core/test/test_ciede_libm_call_args.py
invariant: ciede.c builds in libvmaf_ciede_static_lib with vmaf_libm_call_args; clang keeps its powf() calls, GCC unchanged.
---
<!-- markdownlint-disable MD013 -->
# `ciede.c` calls the C library's `powf()` (ADR-1467)

- `get_r_sub_t()` writes `powf(degrees, 2)`. GCC emits call. clang folds to `degrees * degrees` (correctly rounded). glibc `powf` not correctly rounded on ties (0.12 % of arguments) -> clang build != GCC build, up to 2.0e-11 in `ciede2000`.
- GCC build = reference. Fix lives in `core/src/meson.build`, not in source: `libvmaf_ciede_static_lib` holds `ciede.c` alone, `c_args` = `vmaf_cflags_common + vmaf_strict_fp_args + vmaf_libm_call_args`.
- `vmaf_libm_call_args` (block `# BEGIN VMAF libm call policy`): `['-fno-builtin-powf']` for `clang`, `apple-clang`; `[]` for every other id. GCC object stays byte-identical: no flag for GCC.
- icx excluded on purpose: links Intel libm, differs from GCC build anyway; icx fold = value GPU twins compute.
- Never write `degrees * degrees` in `ciede.c`: moves GCC build on 65 of 180 measured frames.
- Never move `ciede.c` back into `libvmaf_feature_sources`. Never spell `-fno-builtin-powf` outside policy block.
- `test_ciede_powf_call` and `test_ciede_device_math` take same list (`core/test/meson.build`); second one guards library object (fails 640x360 16-bit case when library folds).
- gates: `python3 core/test/test_ciede_libm_call_args.py`; `python3 scripts/ci/run_meson_test.py -- -C build test_ciede_powf_call test_ciede_device_math test_ciede_libm_call_args`.
- upstream change to `get_r_sub_t()`: mirror in `core/test/test_ciede_powf_call.c::r_sub_t_library_calls()`, `feature/ciede_ff_math.h::r_sub_t()`, `cuda/integer_ciede/ciede_device.h::ciede_r_sub_t()`.
