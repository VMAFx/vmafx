---
paths:
  - core/src/feature/hip/integer_psnr_hvs/psnr_hvs_score.hip
  - core/src/feature/hip/integer_adm/adm_csf.hip
invariant: Per-thread atomicAdd replaces CUDA per-warp shfl_down_sync reduction on HIP.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Per-thread atomicAdd replaces CUDA per-warp `__shfl_down_sync` reduce (ADR-0539)

CUDA twin's `cuda_helper.cuh::warp_reduce` hard-codes
`warpSize == 32` in `__shfl_down_sync(0xffffffff, …)` mask. AMD wavefronts **64
wide** on every GCN / CDNA / RDNA target we ship to (gfx906 / gfx90a
/ gfx10 / gfx11); CUDA shuffle pattern incorrect on AMD even when
`__shfl_down_sync` available.

**Established pattern** when porting CUDA kernel ending in
`warp_reduce(accum) + per-warp atomicAdd`:

1. Drop warp reduce entirely.
2. Have **every thread** call `atomicAdd((uint64_cu *)&accum_global[band], lane_value)`.
3. Bit-exact w.r.t. CUDA twin since unsigned 64-bit integer addition
   associative and commutative — only reduction *order* changes.
4. Works on every AMD wavefront width without `#ifdef`-ing per arch.

`atomicAdd` on `unsigned long long` native on gfx90a / gfx10 /
gfx11, falls back to CAS loop on older GCN — HIP runtime handles
arch selection.

Precedent: `integer_vif/vif_statistics.hip` (ADR-0537). pattern holds
only where each thread adds unrounded value.

**Integer ADM is explicit exception**, contrast masking (ADR-1167) and
denominator (ADR-1423) alike: its rounding shift is non-distributive, so
`integer_adm/adm_cm.hip` must first reduce complete row and call
`adm_cm_round_row_total()` exactly once, and `integer_adm/adm_csf_den.hip`
reduces row in shared memory and folds it once. Per-thread or per-wave
rounding followed by `atomicAdd` changes raw accumulator even when
later float score hides it; `adm_csf_den.hip` did that until ADR-1423 and
was 4e-7 off on low-detail frames. device-free
`test_adm_cm_row_rounding_contract.py` pins both HIP contrast-masking
reduction shapes and `test_hip_adm_exact_contract.py` denominator's.
