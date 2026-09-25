- `core/src/feature/sycl/float_adm_sycl.cpp`, `core/src/feature/hip/float_adm_hip.c`,
  `core/src/feature/hip/float_adm/float_adm_score.hip`: resolve open gap
  `T-GAP-FLOAT-ADM-BYPASS-CM-SYCL-HIP-2026-09-07`.
  - Added option declaration `adm_bypass_cm` (`bcm`, int 0..1, default 0) to
    both SYCL and HIP `float_adm` option tables, matching CPU, CUDA, and Metal
    option contracts per ADR-1220.
  - In `float_adm_sycl.cpp`, passed `s->adm_bypass_cm` via `FadmCmParams.bypass_cm` into
    `fadm_cm_threshold()`, returning 0.0f immediately when `bypass_cm != 0`.
  - In `float_adm_hip.c` and `float_adm_score.hip`, passed `s->adm_bypass_cm` via
    `FadmScaleGeom.bypass_cm` and kernel launch `args[]` into `float_adm_csf_cm`
    and `float_adm_aim_cm`, gating the 3x3 contrast-masking threshold calculation
    with `if (bypass_cm == 0)`.
  - Added `test_float_adm_bypass_cm_reaches_kernel()` to both
    `core/test/test_sycl_float_adm_parity.c` and `core/test/test_hip_float_adm_parity.c`,
    verifying within `1e-4` tolerance that `adm_bypass_cm=1` reaches device computation
    and matches CPU `float_adm(adm_bypass_cm=1)`.
  - Verified on hardware: Intel Arc A380 (SYCL 5/5 pass on both small and large fixtures)
    and AMD gfx1036 (HIP 5/5 pass on both small and large fixtures).
