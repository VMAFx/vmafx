---
paths:
  - core/src/feature/sycl/integer_adm_sycl.cpp
  - core/src/feature/sycl/integer_vif_sycl.cpp
  - core/src/feature/sycl/integer_motion_sycl.cpp
  - core/src/feature/sycl/integer_motion_v2_sycl.cpp
  - core/src/feature/sycl/integer_cambi_sycl.cpp
  - core/src/feature/sycl/integer_moment_sycl.cpp
  - core/src/feature/sycl/integer_psnr_sycl.cpp
  - core/src/feature/sycl/integer_psnr_hvs_sycl.cpp
  - core/test/test_sycl_zero_copy_admission.c
  - core/test/test_sycl_zero_copy_model_gate.c
invariant: reads_shared_luma_only() true only if submit() reads no picture for these options (ADR-1688).
---
<!-- markdownlint-disable MD013 MD060 -->
# Zero-copy admission hook

- **`reads_shared_luma_only()` tells truth** ([ADR-1688](../../../../docs/adr/1688-sycl-zero-copy-luma-only-admission.md)).
  `vmaf_read_pictures_sycl()` passes no picture and has luma only on
  device; it admits extractor only when hook answers true for
  parsed options. Hook today: `adm_sycl`, `vif_sycl`, `motion_v2_sycl`,
  `cambi_sycl`, `float_moment_sycl` always true; `motion_sycl` =
  `!motion_add_uv`; `psnr_sycl` / `psnr_hvs_sycl` = `!enable_chroma`. Every
  other SYCL twin has no hook (refused). **On rebase / change**: twin whose
  `submit()` starts reading `ref_pic` / `dist_pic` (host copy of luma, chroma)
  narrows or drops its hook in same PR; twin moved onto shared luma
  may add one. Designated initializer goes after `.provided_features`, before
  `.chars` / `.context_check` (C++ declaration order). Guards:
  `test_sycl_zero_copy_admission` (every SYCL extractor's answer, device-free),
  `test_sycl_zero_copy_model_gate` (refusals and `vmaf_v0.6.1` == CPU, A380).
