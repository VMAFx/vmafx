# ADR-0990: Restore double-precision L/C/S accumulation in CUDA ms_ssim_vert_lcs

- **Status**: Accepted
- **Date**: 2026-06-03
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: cuda, precision, ms-ssim, bit-exactness

## Context

The CUDA kernel `ms_ssim_vert_lcs` in
`core/src/feature/cuda/integer_ms_ssim/ms_ssim_score.cu`
computed the per-pixel luminance (L), contrast (C), and structure (S)
components using `float` literals (`2.0f`) and stored per-block
reduction sums as `float`. The host struct in
`integer_ms_ssim_cuda.c` allocated the partials buffers and pinned
host arrays as `float*`.

The CPU scalar reference,
`ssim_accumulate_default_scalar` in
`core/src/feature/iqa/ssim_tools.c` (lines 183-196), computes the
same quantities using `2.0` (a `double` literal), which under C
standard type-promotion rules promotes `ref_mu * cmp_mu` to double
before the multiply. That means the CPU numerators for L and C are
computed in double, and the final `lv * cv * sv` triple-product is
also in double.

The float accumulation in `ms_ssim_vert_lcs` caused rounding drift
of approximately 0.004 for a 256x144 fixture at scale 0 (approximately
33k pixels), which is roughly 40 times the places=4 tolerance of 1e-4
required by `test_cuda_float_ms_ssim_parity`. The blamed commit is
`8db2715ac2`.

[ADR-0139](0139-ssim-simd-bitexact-double.md) already documented and
fixed the identical pattern for AVX2/AVX-512 SIMD paths. This ADR
applies the same fix to the CUDA backend.

CUDA has supported double-precision `__shfl_down_sync` since sm_30;
all VMAF-supported GPU targets are sm_52 or newer.

## Decision

1. Change the `my_l`, `my_c`, `my_s` local variables in
   `ms_ssim_vert_lcs` from `float` to `double`.
2. Replace `2.0f *` literals with `2.0 *` (double) in the L and C
   numerator expressions, matching the CPU scalar reference exactly.
3. Promote input floats (`ref_mu`, `cmp_mu`, etc.) to double at the
   point of use in the L/C/S expressions, preserving the same
   float-to-double widening that C scalar performs via `2.0 *`.
4. Change the shared-memory warp-partial arrays
   (`s_l_warp`, `s_c_warp`, `s_s_warp`) from `float[...]` to
   `double[...]` so the warp-level and block-level reductions are also
   in double.
5. Change `__shfl_down_sync` operands to `double` (the 64-bit
   overload of `__shfl_down_sync` is available on all sm_30+ devices).
6. Write final block-sums as `double` to the `l_partials`,
   `c_partials`, `s_partials` device buffers.
7. Change the kernel signature `c1`, `c2`, `c3` parameters from
   `float` to `double`.
8. In `integer_ms_ssim_cuda.c`:
   - Change `float c1; float c2; float c3;` in `MsSsimStateCuda` to `double`.
   - Change the constants computation to use double literals.
   - Change `float *h_l_partials[...]` (and `h_c_partials`,
     `h_s_partials`) to `double *`.
   - Change device and pinned-host buffer sizes from
     `sizeof(float)` to `sizeof(double)`.
   - Change the `cuMemcpyDtoHAsync` size from `sizeof(float)` to
     `sizeof(double)`.
   - Remove the `(double)` casts in `collect_fex_cuda` since the
     arrays are already `double`.

The CPU scalar and the intermediate float computations (`ref_mu`,
`ref_sq`, `sigma_xy_geom`, etc.) are unchanged; those remain float
and match the scalar path.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Double reduction (chosen) | Matches scalar C type promotions exactly; closes places=4 gate | Doubles shared-memory footprint for partial arrays; minor extra memory bandwidth on DtoH | **Decision** — only pattern that closes the parity gate without redesigning the caller |
| Kahan compensated float summation | Keeps float throughput; can recover precision | Does not reproduce C's specific `2.0 * float_val` promotion path; complex to verify correctness | Rejected — precision gain is asymmetric and hard to audit |
| Full double pyramid (horiz pass too) | Maximum precision | 2x memory for all intermediate buffers; large performance regression | Rejected — the horiz reduction is not the source of drift; float is sufficient there |
| Accept the places=4 failure, widen tolerance | Zero code change | Contradicts ADR-0139 invariant; silently hides future regressions | Rejected — places=4 is the required gate per ADR-0214 |

## Consequences

- **Positive**: `test_cuda_float_ms_ssim_parity` passes the
  places=4 (1e-4) tolerance gate. CUDA `float_ms_ssim` output
  matches CPU scalar output consistent with ADR-0139's
  double-precision accumulation invariant, extended to the CUDA backend.
- **Negative**: Per-scale partials buffers grow from
  `block_count * 4` bytes to `block_count * 8` bytes (5 scales,
  3 channels, 2 allocation tiers: device + pinned host). At 1080p
  with 16x8 blocks this is approximately 1.1 MB device-side, which
  is negligible relative to pyramid frame buffers.
- **Neutral / follow-ups**:
  - The HIP `integer_ms_ssim_hip.c` and HIP kernel have the same
    `float` partial pattern. A follow-up should apply the same fix.
  - `docs/rebase-notes.md` entry documents the double-partials
    invariant so future upstream ports preserve it.

## References

- Phase-2 diagnosis by agent `w487fr96l` (2026-06-03): float
  accumulation in `ms_ssim_vert_lcs` using `2.0f` literals causes
  approximately 0.004 drift over 33k pixels, 40x the places=4 gate.
- Blamed commit: `8db2715ac2`.
- Related ADR: [ADR-0139](0139-ssim-simd-bitexact-double.md) —
  same `2.0 *` double-promotion fix for AVX2/AVX-512 paths.
- Related ADR: [ADR-0214](0214-gpu-parity-gate-places4.md) —
  places=4 GPU parity gate.
- Scalar reference:
  `core/src/feature/iqa/ssim_tools.c` `ssim_accumulate_default_scalar`.
