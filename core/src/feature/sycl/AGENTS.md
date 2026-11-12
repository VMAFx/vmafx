<!-- GENERATED (`make docs-fragments-write`); do not hand-edit. -->
<!-- markdownlint-disable MD013 MD060 -->
# AGENTS.md — core/src/feature/sycl

Generated index. Pick area for touched paths (both if 2 fit); match `Touching`,
read each matching page first. Text above table binds every file. `Touching`
relative; leading `/` = repo root. New invariant -> `AGENTS.d/` page
([how](../../../../docs/development/agents-index.md)).

Orientation for agents on per-feature SYCL kernels (DPC++).
Parent: [../AGENTS.md](../AGENTS.md). Backend runtime (queue, USM,
dmabuf import) lives one level up in
[`../../sycl/AGENTS.md`](../../sycl/AGENTS.md).

## Scope

```text
feature/sycl/
  <feature>_sycl.cpp           # one TU per kernel: registration + submit/collect + sycl::queue::submit lambda
```

All TUs compiled with `icpx` (Intel oneAPI) — build line
under [`../../meson.build`](../../meson.build) adds `-fsycl` and SYCL
strict FP line (`sycl_strict_fp_args`, ADR-1367) for every per-kernel TU.

## Ground rules

- **Parent rules** apply (see [../AGENTS.md](../AGENTS.md) +
  [../../AGENTS.md](../../AGENTS.md) +
  [`../../sycl/AGENTS.md`](../../sycl/AGENTS.md)).
- **Wholly-new fork files carry `Copyright 2026 Lusoris`**
  per [ADR-0861](../../../../docs/adr/0861-vmafx-copyright-policy-drop-anthropic.md).
  Most TUs here fork-original SYCL ports of
  Netflix CUDA kernels. Upstream-mirrored files keep original headers — no retro-fit.

## Twin-update rules

When SYCL TU has live CUDA, HIP, or Metal twin, user-visible behavior and
numeric fixes must be reviewed across those twins in same PR. Vulkan was
removed in ADR-0726 and is not live twin. complete CUDA mapping lives in
[`../cuda/AGENTS.md`](../cuda/AGENTS.md). cross-backend parity gate at
`places=4`
([`scripts/ci/cross_backend_parity_gate.py`](../../../../scripts/ci/cross_backend_parity_gate.py),
ADR-0214) catches drift only after full GPU run; it does not replace that
source review.

## Rebase-sensitive invariants

## Areas

| Area | Read first | Covers |
| --- | --- | --- |
| ADM and VIF twins | [adm-and-vif](AGENTS-adm-and-vif.md) | integer_adm_sycl.cpp, float_adm_sycl.cpp, sycl_float_adm_math.h, integer_vif_sycl.cpp, sycl_integer_vif_math.h, float_vif_sycl.cpp, sycl_float_vif_math.h, core/test/test_sycl_adm_parity.c, core/test/test_sycl_float_adm_parity.c, core/test/test_sycl_vif_parity.c, core/test/test_sycl_float_vif_parity.c; ADM AIM pass, int16 scale 0, viewing distances, VIF odd-width stride, bit-exact float ADM/VIF. |
| Build, FP, tidy, and parity | [build-and-parity](AGENTS-build-and-parity.md) | core/meson.build, core/meson_options.txt, core/src/meson.build, sycl_compat.h, sycl_exact_fp.h, sycl_soft_double.h, sycl_soft_signed.h, .clang-tidy, scripts/ci/clang-tidy-sycl.sh, scripts/ci/gen-sycl-compile-commands.py, scripts/ci/exact_twins.d/*, scripts/ci/cross_backend*.py, core/test/test_sycl_exact_twins.c, core/test/test_sycl*parity.c; strict-FP, soft-fp64, orientation. |
| Motion, PSNR, and moment twins | [motion-psnr-and-moment](AGENTS-motion-psnr-and-moment.md) | integer_motion*, float_motion_sycl.cpp, integer_psnr_sycl.cpp, float_psnr_sycl.cpp, integer_psnr_hvs_sycl.cpp, integer_moment_sycl.cpp, core/test/test_sycl_motion*, core/test/test_sycl_float_motion_parity.c, core/test/test_sycl_twin_option_parity.c, core/test/test_sycl*psnr*.c, core/test/test_sycl_float_moment_parity.c; SAD, 5-frame window, PSNR-HVS. |
| Queues, device frames, and kernels | [runtime-and-kernels](AGENTS-runtime-and-kernels.md) | integer*.cpp, float*.cpp, speed*, ssimulacra2_sycl.cpp, sycl_compat.h, sycl_tile_index.h, core/src/sycl/dmabuf_import.cpp, core/src/feature/cuda/integer*.c, core/test/vmafx_sycl_cells.h, core/test/test_sycl_init_unwind.cpp, core/test/test_sycl_zero_copy*.c; device pictures, VAAPI/dmabuf zero-copy, queue sync, init unwind, scratch/local memory, sub-groups, tile borders, option sync. |
| SSIM, CAMBI, SpEED, and metrics | [ssim-cambi-and-metrics](AGENTS-ssim-cambi-and-metrics.md) | integer_ssim_sycl.cpp, integer_ms_ssim_sycl.cpp, integer_cambi_sycl.cpp, integer_ciede_sycl.cpp, sycl_ciede_math.h, speed*, sycl_speed_cov_math.h, ssimulacra2_sycl.cpp, sycl*ssim*.h, core/test/test_sycl*ssim*.c, core/test/test_sycl_cambi_parity.c, core/test/test_sycl_ciede_parity.c, core/test/test_sycl_speed*; decimation, CAMBI options, CIEDE fp32, SpEED cov, IIR. |
