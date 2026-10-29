<!-- markdownlint-disable MD013 MD060 -->
# ADR-3383: Device twins take odd bit depths only from a measured allow-list

- **Status**: Accepted
- **Date**: 2026-10-28
- **Deciders**: maintainer (milestone 0.1.0 order of 2026-10-27); RC4 WP13 (#2476)
- **Tags**: api, rc4, correctness, gpu

## Context

[ADR-2164](2164-vmafx-odd-bit-depths-float-psnr-ciede.md) left
`refuse_unscaled_bpc()` (`core/src/feature/feature_extractor.cpp`) as a
deny-list: it refused 9, 11, 13, 14 and 15 bits for the device twins of the
five `picture_copy()` families and let every other twin through. Twins outside
that list were never compared with the CPU at those depths either. One of them
is wrong: the CUDA, HIP and SYCL `float_moment` twins scale samples with a
factor that is 1.0 at every depth but 10, 12 and 16 bits
(`moment_cuda_scaler()`, `moment_hip_scaler()`, the SYCL host), so at 9, 11,
13, 14 and 15 bits they return the unscaled moments while the CPU
(`picture_copy()`, ADR-2164) scales them. Issue #2476 asks that the library
refuse depths other than 8, 10, 12 and 16 by name until the twin matrix passes.

## Decision

1. `refuse_unscaled_bpc()` is an allow-list. A device twin (CUDA, SYCL, HIP,
   Metal) at a depth other than 8, 10, 12 or 16 bits returns `-EINVAL` and
   logs `<extractor>: picture bit depth N is not supported on this backend`,
   unless it is listed as measured at that depth.
2. The list (`kMeasured`) holds exact extractor names, each with the highest
   depth its parity test carries: `float_psnr_cuda`, `float_psnr_sycl`,
   `float_psnr_hip`, `ciede_cuda`, `ciede_sycl` and `ciede_hip` up to 15 bits
   (`float_psnr_twin_odd_depths_exact()`, `ciede_twin_odd_depths()`,
   ADR-2164); `psnr_hvs_cuda`, `psnr_hvs_sycl`, `psnr_hvs_hip` and
   `integer_psnr_hvs_metal` up to 11 bits
   (`hvs_twin_every_depth_identical()`). No prefix match: an unlisted twin of
   a listed family, such as `float_psnr_metal`, is refused.
3. A twin joins the list by exact name in the pull request that adds the
   depths to its parity test, with every cell equal to the CPU (or within its
   declared `LIBM_TWINS` bound).
4. CPU extractors are unaffected.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Allow-list of exact twin names with a maximum depth (chosen) | Fails closed; a new twin is refused until measured; measured depths stay open | One line per twin to maintain | Chosen |
| Allow-list of family name prefixes | Shorter list | Lets the unmeasured `float_psnr_metal` and `integer_ciede_metal` through and refuses the measured `psnr_hvs` depths 9 and 11 | Rejected: the list must name what was measured |
| Extend the deny-list with `float_moment` | Smallest change | Every future unmeasured twin is accepted silently again | Rejected: the defect class repeats |
| Fix the `float_moment` scalers only | Removes the known wrong score | No device evidence for the other twins at odd depths | Not enough on its own; the scaler fix follows with its parity cells |

## Consequences

- **Positive**: no device twin returns a score at an odd depth without parity
  evidence; the `float_moment` wrong scores at odd depths are refused.
- **Negative**: the `psnr_hvs` twins are refused at 13, 14 and 15 bits and the
  Metal `float_psnr` and `ciede` twins at every odd depth until measured; the
  CPU extractors still score them.
- **Neutral / follow-ups**: fix the `float_moment` twin scalers and add odd
  depths to the parity tests of each twin family; state row
  `T-FLOAT-EXTRACTORS-ODD-DEPTH-TWINS-UNMEASURED-2026-10-07`.

## References

- [ADR-2164](2164-vmafx-odd-bit-depths-float-psnr-ciede.md).
- Issue #2476 (RC4 WP13, input formats), acceptance: refuse other depths by name until the twin matrix passes (paraphrased).
- Tests: `core/test/test_read_pictures_bpc.c` (`test_unmeasured_twins_refuse_odd_depths`, `test_measured_twins_keep_odd_depths`).
