<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1154: AMD ROCm HIP Backend Gap Closure and Extractor Promotion

- **Status**: Accepted
- **Date**: 2026-09-03
- **Deciders**: Kilian, Antigravity Agent
- **Tags**: hip, rocm, gpu, dispatch, parity, docs

## Context

An architectural audit of the AMD ROCm HIP backend in VMAFx identified 7 operational
gaps across feature extractor dispatch, kernel argument packaging, runtime configuration,
dead source files, and documentation:

1. **Unpromoted HIP Feature Extractors (`GAP-HIP-UNFLAGGED-FEATURE-EXTRACTORS`)**:
   13 of 19 registered HIP feature extractors left `VMAF_FEATURE_EXTRACTOR_HIP` cleared
   (`.flags = 0`), silently falling back to CPU execution when models were evaluated with
   `--backend hip`.
2. **Driver API Argument Packaging Defects (`GAP-HIP-KERNEL-ARG-PACKAGING`)**:
   Under `hipModuleLaunchKernel`, arguments must be passed as addresses to the argument
   storage (`&var`). Several extractors passed device pointers directly rather than
   the address of the pointer variable (e.g. `s->rb.device` in `float_psnr_hip.c`), causing
   immediate NULL-pointer dereference faults on GPU. `float_moment_hip.c` transposed
   the stride and distorted picture pointer arguments.
3. **Type Width and Buffer Size Discrepancies (`GAP-HIP-MS-SSIM-TYPE-MISMATCH`)**:
   In `integer_ms_ssim_hip.c`, the host state defined `c1..c3` as `float` and allocated
   partial buffers with `sizeof(float)` elements, whereas the underlying `ms_ssim_vert_lcs`
   kernel (`ms_ssim_score.hip`) expects `double c1..c3` and writes `double *` partials,
   leading to bit corruption and buffer overflow during kernel execution and DtoH copy.
4. **Option Dictionary Serialization Timing (`GAP-HIP-CAMBI-DICT-TIMING`)**:
   In `integer_cambi_hip.c`, `vmaf_feature_name_dict_from_provided_features` was invoked
   after writing resolution defaults to `s->full_w` and `s->full_h`, causing the dictionary
   generator to interpret resolution defaults as user overrides (`cambi_full_w_576_full_h_324`),
   breaking canonical feature lookup.
5. **Partial Plane Memory Transfer (`GAP-HIP-PSNR-CHROMA-UNINITIALIZED`)**:
   `integer_psnr_hip.c` only copied plane 0 in `submit_fex_hip`, leaving planes 1 and 2
   uninitialized when `enable_chroma=true` was specified.
6. **Numerical and Buffer Prerequisites on SSIM and ADM (`GAP-HIP-SSIM-ADM-DEFERRED`)**:
   `integer_ssim_score.hip` was ported from the 11-tap float Gaussian rather than the
   9-tap int64 separable kernel, drifting by 4.5e-3 from the CPU reference (violating
   ADR-0564 bit-exact ground truth). `integer_adm_hip.c` lacked host-to-device picture
   staging buffers, passing host pointers directly into device kernels.
7. **Dead Source Files and Unimplemented Dispatch Query (`GAP-HIP-DEAD-FILES-DISPATCH`)**:
   `core/src/feature/hip/integer_adm/adm_decouple.hip` and orphan `integer_moment_hip.h` /
   `moment_score.hip` were uncompiled. `vmaf_hip_dispatch_supports()` returned 0
   unconditionally without checking features or `VMAF_HIP_DISPATCH`.

## Decision

We resolve the HIP backend gap inventory in a unified change:

1. **Kernel Argument and Parameter Fixes**:
   - Fixed pointer-to-pointer packaging in `float_psnr_hip.c` (`&partials_dev`) and
     `float_moment_hip.c` (`&sums_dev`).
   - Fixed argument ordering in `float_moment_hip.c` (`ref, dis, ref_stride, dis_stride`).
   - Promoted `c1..c3` and partial buffers in `integer_ms_ssim_hip.c` to `double` and
     `sizeof(double)` matching `ms_ssim_vert_lcs` in `ms_ssim_score.hip`.
   - Moved `vmaf_feature_name_dict_from_provided_features` ahead of dimension defaults in
     `integer_cambi_hip.c`.
   - Iterated over all `s->n_planes` in `integer_psnr_hip.c::submit_fex_hip`.
2. **Feature Extractor Promotion**:
   - Promoted 11 extractors (`integer_cambi_hip`, `ciede_hip`, `integer_psnr_hip`,
     `float_psnr_hip`, `float_moment_hip`, `integer_motion_v2_hip`, `float_motion_hip`,
     `float_ssim_hip`, `integer_ms_ssim_hip`, `integer_psnr_hvs_hip`, `float_adm_hip`)
     to active GPU execution (`VMAF_FEATURE_EXTRACTOR_HIP` / `TEMPORAL`), bringing active
     HIP extractors to 17 of 19. All 17 pass device parity tests against CPU reference.
3. **Formal Deferral of Divergent / Unstaged Extractors**:
   - `integer_ssim_hip` retains `.flags = 0` per ADR-0564 until the 9-tap int64 kernel lands.
   - `integer_adm_hip` retains `.flags = 0` until picture staging buffers (~350 LOC) or
     the HIP device picture pool (T7-10c) lands.
4. **Dead File Pruning**:
   - Removed `adm_decouple.hip`, `integer_moment_hip.h`, and `integer_moment/moment_score.hip`.
5. **Dispatch Strategy & Runtime Integration**:
   - Implemented `vmaf_hip_dispatch_supports()` with a comprehensive `g_hip_features` lookup
     table and `VMAF_HIP_DISPATCH` environment support via `vmaf_gpu_dispatch_env_get`.
   - Drained `gpu_pending` in `libvmaf.c::flush_context_serial` for non-CUDA/SYCL extractors.
   - Added informative `vmaf_log` messages naming `-Denable_hipcc=true` in `!HAVE_HIPCC` stubs.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Promote `integer_ssim_hip` with relaxed tolerance** | Reaches 18/19 active GPU extractors | Silent 4.5e-3 divergence vs CPU ground truth on canonical `"ssim"` | Violates ADR-0564; CPU fallback preserves numerical correctness |
| **Implement HIP device picture pool (T7-10c) immediately** | Enables zero-copy host-to-device picture management across all extractors | High complexity (>600 LOC) requiring ROCm external memory and DMA-BUF plumbing | Out of scope for gap closure; internal staging buffers provide immediate working GPU paths |
| **Retain `vmaf_hip_dispatch_supports` stub returning 0** | Zero code churn in dispatch strategy | External callers cannot probe HIP capability; `VMAF_HIP_DISPATCH` env overrides ignored | Defeats feature dispatch parity across CUDA, SYCL, and HIP backends |

## Consequences

- **Positive**:
  - 17 of 19 registered HIP feature extractors actively dispatch on AMD GPU hardware.
  - Parity verified on AMD Granite Ridge / Raphael iGPU (`gfx1036`).
  - Argument packaging faults, type width mismatches, and memory corruptions resolved.
  - Dead and orphan files pruned from the tree.
- **Negative**:
  - `integer_ssim` and `integer_adm` fall back to CPU execution under `--backend hip` until their replacement kernels land.
- **Neutral / follow-ups**:
  - Porting the 9-tap int64 integer SSIM kernel (`integer_ssim_score.cu`) and the HIP device picture pool (T7-10c) remain tracked in `docs/state.md`.

## References

- ADR-0212: AMD HIP backend scaffold
- ADR-0214: Cross-backend GPU parity CI gate
- ADR-0530: HIP feature flag promotion and picture buffer type
- ADR-0533: HIP extractor registration sweep
- ADR-0564: Integer SSIM GPU real kernels
- ADR-0858: C++23 isolated static library for `gpu_dispatch_env`
- Research-1147: HIP Backend Gap Resolution and AMD iGPU Parity
