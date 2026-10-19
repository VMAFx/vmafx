---
paths:
  - core/meson.build
  - core/src/meson.build
  - core/src/feature/adm_csf_tools.h
  - core/src/feature/adm_tools.c
  - core/src/feature/adm_tools.h
  - core/src/feature/barten_csf_tools.h
  - core/src/feature/ciede.c
  - core/src/feature/integer_adm.h
  - core/src/feature/integer_ssim.c
  - core/src/feature/speed.c
  - core/src/feature/speed_internal.c
  - core/src/feature/speed_qa.c
  - core/src/feature/vif_tools.c
  - core/src/feature/y_funque_plus.c
  - core/src/feature/sycl/float_adm_sycl.cpp
  - core/src/feature/sycl/integer_adm_sycl.cpp
invariant: No TU defines M_PI / M_E; <math.h> provides them, Windows via meson's -D_USE_MATH_DEFINES.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Math constants come from `<math.h>` (Netflix/vmaf 4e150067b)

- No TU defines `M_PI`, `M_E` or `_USE_MATH_DEFINES`. Linux: `_GNU_SOURCE`;
  macOS: `_DARWIN_C_SOURCE`; Windows (MSVC, clang-cl, icx-cl, MinGW-w64):
  `-D_USE_MATH_DEFINES`; all project arguments in `core/meson.build`. nvcc:
  `-D_USE_MATH_DEFINES` in `cuda_flags`. Command-line define precedes every
  `<math.h>` include; per-file `#define` gives no such guarantee.
- Windows define = one list, `vmaf_math_constant_args` (empty off Windows).
  icpx custom targets never see project arguments, so `sycl_common_args` +
  `sycl_feature_tail_args` (`core/src/meson.build`) add list; every icpx
  compile line takes one of two lists (`test_sycl_math_constants_contract.py`).
  msvcism stage counts define as project-wide only with both SYCL lists.
  Missing = Windows SYCL zip fails on `M_PI`
  (T-SYCL-WINDOWS-M-PI-UNDECLARED-2026-10-09).
- Removed literals (`3.14159265358979323846`, `...264338327`,
  `3.141592653589793238462643`) = same double as glibc / MinGW `M_PI`
  (`0x1.921fb54442d18p+1`); x86 object code identical apart from moved line
  numbers.
- **On upstream sync**: file bringing back `#ifndef M_PI` /
  `#define _USE_MATH_DEFINES` takes fork's side (delete guard). Upstream spells
  `add_project_arguments('-D_USE_MATH_DEFINES', ...)` literally; keep fork's
  list form.
