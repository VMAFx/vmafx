<!-- markdownlint-disable MD013 MD060 -->
# Research-0764: psnr_hvs CUDA kernel — F3 `__ldg()` + `__launch_bounds__(64)` (2026-05-29)

**Context**: PR #96 candidate #5 — applies the F3 struct-by-value fix to the
`psnr_hvs` kernel in
`core/src/feature/cuda/integer_psnr_hvs/psnr_hvs_score.cu`.

## Background — F3 pattern

ADR-0754 (PR #93) established the pattern: extract `const float *__restrict__`
pointers from `VmafCudaBuffer` struct arguments before inner loops, then use
`__ldg()` for all reads. Passing `VmafCudaBuffer` by value hides the underlying
device pointer from the nvcc compiler's non-coherent-load analysis; the
extraction makes the alias-free invariant visible, routing loads through the L1
read-only texture cache (LDG.E.CONSTANT instruction in SASS).

ADR-0757 (PR #96 fix #2) applied the same pattern to `ms_ssim_horiz` and
`ms_ssim_vert_lcs`, both 128-thread kernels with K=11 inner loops.

## psnr_hvs kernel specifics

| Property | Value |
|---|---|
| Block config | 8x8 = 64 threads |
| Loads per block | 64 reads from `ref_buf` + 64 reads from `dist_buf` = 128 total |
| Loop structure | Cooperative tile load (all 64 threads load 1 element each) |
| Input buffers | 2 (ref, dist) |
| `__ldg()` calls added | 2 (one per pointer, one element per thread) |

## Change summary

- `VmafCudaBuffer ref_in, dist_in` remain by-value in the kernel signature
  (ABI unchanged).
- `const float *__restrict__ ref_buf` and `const float *__restrict__ dist_buf`
  extracted from `.data` once before the `if (valid_block)` branch.
- `__ldg(&ref_buf[src_idx])` and `__ldg(&dist_buf[src_idx])` used for the
  two element loads inside the valid-block guard.
- `__launch_bounds__(64)` added — matches the actual 8x8 block launch in
  `integer_psnr_hvs_cuda.c`; mirrors ADR-0754 / ADR-0757 pattern.

## Predicted performance

Based on ADR-0754 live ncu measurements on RTX 4090 sm_89:

| Resolution | Expected change | Confidence |
|---|---|---|
| 576p | noise-dominated (wave-limited regime, < 0.5 waves) | low |
| 1080p | -3 to -5% kernel duration (DRAM-bound regime) | medium |
| 4K | -5 to -8% kernel duration (deep DRAM-bound, multi-wave) | medium |

The `psnr_hvs` kernel loads 2 buffers vs `vert_combine`'s 5, so the absolute
L2-pressure reduction is proportionally smaller. However the 64-thread block
configuration (vs 128 for SSIM) means higher occupancy sensitivity to register
spilling — `__launch_bounds__(64)` provides the budget guard.

## Correctness

No arithmetic is changed. The cooperative tile load pattern and all subsequent
shared-memory reductions, DCT passes, and float accumulations are untouched.
Scores must be bit-identical to the pre-patch kernel (ADR-0214 places=4 gate).

## Live ncu A/B measurements

Pending — to be filled in after container build + ncu run.

## Verdict

READY — same pattern as ADR-0754 / ADR-0757, no arithmetic change, no ABI
change, bit-identical scores expected.
