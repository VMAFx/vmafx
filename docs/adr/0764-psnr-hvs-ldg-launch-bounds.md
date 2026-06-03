<!-- markdownlint-disable MD013 MD060 -->
# ADR-0764: psnr_hvs CUDA kernel — `__ldg()` + `__restrict__` + `__launch_bounds__(64)`

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `cuda`, `perf`, `psnr_hvs`, `fork-local`

## Context

The `psnr_hvs` CUDA kernel in
`core/src/feature/cuda/integer_psnr_hvs/psnr_hvs_score.cu` (PR #96 candidate #5)
accepted two `VmafCudaBuffer` input arguments (`ref_in`, `dist_in`) by value.
Passing the struct by value hides the underlying device pointer from the nvcc
compiler's non-coherent-load analysis: the compiler cannot prove the pointer
is read-only and alias-free, so it emits generic LD.E loads instead of the
more efficient LDG.E.CONSTANT path through the L1 read-only texture cache.

The kernel loads a 64-pixel tile (one 8×8 block) per thread cooperatively —
64 total loads for `ref_buf` and 64 for `dist_buf`, reaching 128 reads of
global device memory per block per frame per plane.

The same fix (F3 in the PR #96 audit) was previously applied to
`calculate_ssim_vert_combine` (ADR-0754, PR #93) and to `ms_ssim_horiz` /
`ms_ssim_vert_lcs` (ADR-0757, PR #96 fix #2), establishing a stable pattern.

The kernel's block configuration is 8×8 = 64 threads. Adding
`__launch_bounds__(64)` gives nvcc a precise occupancy hint, matching the
actual dispatch in the host glue.

## Decision

Extract `const float *__restrict__` pointers from `ref_in.data` and
`dist_in.data` once before the cooperative tile load, then apply `__ldg()`
to all 128 per-thread element reads. Add `__launch_bounds__(64)` to the kernel
declaration. No arithmetic is changed; no score path is altered.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Leave as-is | No churn | Misses L1 read-only cache; generic loads at >=1080p add L2 pressure | F3 pattern already established across all F3 candidates in the audit |
| Change kernel signature to raw pointers | Eliminates struct overhead entirely | Breaks the established VmafCudaBuffer calling convention shared with the host-glue template | Extraction achieves the same effect with zero API change |
| `__ldg()` only, no `__restrict__` | Simpler | `__restrict__` is what makes the alias-free invariant visible to the compiler's PTX emission analysis | Both are required together for the LDG.E.CONSTANT path |

## Consequences

- **Positive**: All 128 tile reads (64 ref + 64 dist) routed through the
  L1 read-only texture cache. Predicted -3 to -5% kernel duration at >=1080p
  where the combined input tile footprint exceeds L2 capacity
  (mirrors ADR-0754 1080p measurement of -4.2%).
- **Negative**: None — no arithmetic change; scores are bit-identical
  (ADR-0214 places=4 gate).
- **Neutral / follow-ups**: `__launch_bounds__(64)` retained as a register-
  budget guard even if current register pressure does not trigger spilling
  (mirrors ADR-0754 note on `__launch_bounds__(128)` for `vert_combine`).

## References

- [ADR-0754](0754-cuda-ssim-vert-combine-ldg-pinned-leak.md) — first application of this pattern (`calculate_ssim_vert_combine`).
- [ADR-0757](0757-cuda-ms-ssim-vert-lcs-horiz-ldg.md) — second application (`ms_ssim_horiz` + `ms_ssim_vert_lcs`).
- [ADR-0756](0756-cuda-kernel-f3-struct-value-audit.md) — PR #96 audit that identified `psnr_hvs` as candidate #5.
- [ADR-0743](0743-cuda-vif-filter1d-ldg-launch-bounds.md) — original `__ldg()` + `__launch_bounds__` precedent on VIF filter1d.
- [ADR-0214](0214-cross-backend-parity-gate.md) — cross-backend parity gate (places=4).
- PR #96 candidate #5; PR #93 reference implementation.
- Source: req — "Apply the F3 fix (`__restrict__` raw pointers + `__ldg()`) to `psnr_hvs` kernel. Mirror PR #93 pattern."
