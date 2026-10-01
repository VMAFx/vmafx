<!-- markdownlint-disable MD013 MD060 -->
# ADR-1402: Integer ADM keeps the scale-0 masking centre tap in int32 and clamps the excess in int64

- **Status**: Accepted
- **Date**: 2026-10-01
- **Deciders**: lusoris
- **Tags**: adm, correctness, numerics, simd, cuda, hip, sycl, metal, upstream-divergence, fork-local

## Context

Integer ADM masks each scale-0 coefficient with a threshold: the 3x3 sum of the
filtered neighbours in the three bands, with the centre tap replaced by 1/15 of
the coefficient itself, `((8738 * |a|) + 2048) >> 12`. That tap reaches 69904
for `|a| = 32768`. Upstream's C reference narrows it to `int16_t`, so every tap
above 32767 (`|a| >= 15360`) wraps negative. The contrast kept after masking is
`max(|x| - (thr << shift), 0)`.

The fork carried the first revision of its own upstream pull request
Netflix/vmaf#1602, which made the AVX2 and AVX-512 kernels and every GPU twin
reproduce the wrap so that all implementations agreed
(`T-ADM-CM-SIMD-NOISE-NOT-BIT-EXACT-2026-09-18`,
`T-SYCL-ADM-INT16-SEMANTICS-2026-09-18`). After review upstream, the second
revision of that pull request removes the wrap instead. Two measurements on
master `c7f28317f` (2026-10-01) show why the wrap is a defect and not a
convention:

- A wrapped tap that outweighs its eight neighbours makes the threshold
  negative, and a negative threshold adds contrast instead of masking it. A
  flat grey 64x64 reference against the same picture with isolated 4x2 patches
  scored `integer_adm_scale0` 1.0829225419556654 and `integer_adm2`
  1.035481944668303, where `float_adm` gives exactly 1 for both. A ratio above
  1 says the distorted picture has more of the reference's detail than the
  reference.
- `thr << shift` with a negative `thr` is undefined in C. PR #1662 removed it
  from the scalar reference; the scalar tails of `adm_avx2.c` and
  `adm_avx512.c` kept it, and a UBSan build stopped on a 24x24 picture with one
  patch at (3, 3) (`adm_avx512.c:2291`, `adm_avx2.c:2687`).

The maintainer decided to remove the wrap in every implementation now, on the
condition that no Netflix golden assertion moves (see References).

## Decision

The centre tap stays in int32 in every implementation, and the excess over the
threshold is `clamp(|x| - thr * 2^shift, 0, INT32_MAX)`, formed in int64. The
scalar definition is `adm_cm_thresh()` in
`core/src/feature/integer_adm_kernels.h` and `adm_cm_excess_s0()` in
`core/src/feature/adm_cm_accumulator.h`; they are the second revision of
upstream's #1602 for the scalar code. Each twin returns the scalar's value bit
for bit:

| Implementation | Where | How the excess is formed |
|---|---|---|
| Scalar, x86 edge rows and tail columns | `adm_cm_accum_round()` | calls `adm_cm_excess_s0()` |
| AVX2 | `cm_excess_avx2()`, `cm_row_avx2()` in `x86/adm_avx2.c` | a row is summed in int32 (exact while every threshold lies in [0, 2^19)); a row with a threshold outside that range is summed again with the threshold clamped to +/-2^(31 - shift), int32 for a non-negative threshold and saturating uint32 for a negative one |
| AVX-512 | `cm_excess_avx512()`, `cm_row_avx512()` in `x86/adm_avx512.c` | the same, with a lane mask |
| CUDA (DLM and AIM) | `cuda/integer_adm/adm_cm.cu` | calls `adm_cm_excess_s0()` |
| HIP (DLM) | `hip/integer_adm/adm_cm.hip` | calls `adm_cm_excess_s0()` |
| SYCL (DLM and AIM) | `adm_dev_cm_excess_s0()` in `sycl/integer_adm_sycl.cpp` | int64, as the scalar |
| Metal (DLM and AIM) | `adm_cm_excess_s0()` in `metal/integer_adm.metal` | MSL twin in `long`; source only, no device was available |

The vector kernels also narrow the squared excess to int32 and shift the cube
arithmetically, as the scalar does: AVX-512 with `_mm512_sra_epi64`, AVX2 by
folding the sign bias into the rounding term (`cm_accum_avx2()`), which costs
no instruction per sample. A decoded picture never takes the second pass of a
row: its filtered neighbours are non-negative and its thresholds far below
2^19. NEON has no contrast-masking kernel; aarch64 runs the scalar code.

The x86 files could only be edited after their oversized functions were split
([ADR-1298](1298-hiss-audit-merge-touched-scope.md)); that refactor is the
first commit of the same pull request and changes no score.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep the int16 wrap everywhere (first revision of #1602) | Equal to upstream master on every input; nothing to re-verify | Scores isolated impairments above 1; depends on a narrowing conversion the C standard leaves to the implementation; a negative threshold needs modular arithmetic in every twin | The wrap is a defect, and upstream's own review rejected it |
| Remove the wrap only when upstream merges #1602 | No period in which the fork differs from upstream master | The defect and the open state rows stay until a date the fork does not control | The maintainer chose to fix now, provided the goldens hold; they do |
| Upstream's vector form: `thr > (INT32_MAX >> shift)` selects 0, otherwise the 32-bit expression | Fewer vector instructions | Equals upstream's scalar only for a non-negative threshold; a negative threshold whose product leaves int32 wraps where the scalar saturates | The fork's rule is that SIMD equals the scalar on every input the kernel accepts (ADR-0138, ADR-0139); `test_integer_adm_simd` fails on this form |
| Saturate the centre tap at 32767 instead of widening it | The threshold stays in int16 range | A third behaviour, equal to neither upstream revision; coefficients above 15359 would be under-masked | No reference implementation to compare against |
| Clamp the excess in int32 after a saturating threshold product | No 64-bit arithmetic in the scalar | More branches than the int64 form and no faster once the int64 clamp is two selects | The int64 form is upstream's and is simpler to mirror in device code |

## Consequences

- **Positive**:
  - Integer ADM no longer scores above 1 on isolated impairments: the 64x64
    patch picture gives `integer_adm_scale0` 1 and `integer_adm2` 1 (before:
    1.0829225419556654 and 1.035481944668303), the 24x24 one 1 and 1 (before:
    1.0701309766616138 and 1.0301034698295983), on scalar, AVX2, AVX-512, CUDA,
    HIP and SYCL.
  - No implementation left-shifts a negative threshold or depends on an int16
    narrowing conversion in this path.
  - The Netflix golden gate is unchanged: 271 passed, 12 skipped before and
    after (`make test-netflix-golden` profile, gcc 16.2.1). The 13 fixture
    pairs under `python/test/resource/yuv/` give identical JSON at
    `--precision max` with the default model, `adm` and `float_adm` on the
    CPU. The CUDA, HIP and SYCL twins (`adm_cuda`, `adm_hip`, `adm_sycl`) give
    identical JSON on the same 13 pairs before and after.
- **Negative**:
  - The fork's integer ADM differs from upstream master on content that reaches
    a centre coefficient of 15360 or more, until Netflix/vmaf merges #1602.
    Measured (CPU, default options, pooled mean): independent full-range 8-bit
    noise at 576x324 moves `integer_adm2` from 0.38954843646923215 to
    0.38950305912838107 and `integer_adm_scale0` from 0.4549724278265123 to
    0.45480076976959144; noise against the same noise plus a perturbation in
    [-16, 15] moves `integer_aim` from 7.166752298663627e-05 to 0 and
    `integer_adm3` from 0.9771812019019719 to 0.9772170356634652. The default
    model's own ADM features (`adm_csf_mode=2`) and its VMAF score did not move
    on either. One-pixel stripes, salt-and-pepper impulses on a gradient and
    blurred blocks are identical.
  - The scalar excess is formed in int64 with two selects; the scalar
    scale-0 contrast-masking stage takes about 5% longer and the scalar
    extractor as a whole is unchanged within measurement noise. The research
    digest has the stage times of every dispatch level.
- **Neutral / follow-ups**:
  - `docs/rebase-notes.md` records the divergence and what to do when upstream
    merges or changes #1602.
  - The Metal twin was changed in source only; its parity tests need an Apple
    device.
  - The golden gate must be re-run when upstream changes the centre tap or the
    excess again.

## References

- Q (popup answer, 2026-10-01, to "remove the int16 centre-tap wrap in every
  implementation now, or wait for upstream?"): "Fix now if goldens hold
  (Recommended)".
- [Netflix/vmaf#1602](https://github.com/Netflix/vmaf/pull/1602), both
  revisions, and Netflix/vmaf issue 1610 (the AVX2 cube shift).
- [Research-1402](../research/1402-adm-cm-centre-tap-int32.md): golden gate,
  score deltas, the vector derivation, planted defects, device parity and
  stage timings.
- [ADR-1298](1298-hiss-audit-merge-touched-scope.md) (touched-file rule),
  [ADR-0141](0141-touched-file-cleanup-rule.md) (refactor first),
  [ADR-0138](0138-iqa-convolve-avx2-bitexact-double.md) and
  [ADR-0139](0139-ssim-simd-bitexact-double.md) (SIMD equals scalar),
  [ADR-1167](1167-adm-cm-row-level-rounding.md) (the row fold these kernels
  share), [ADR-0024](0024-netflix-golden-preserved.md) (golden gate).
- `docs/state.md`: `T-ADM-CM-CENTRE-TAP-WRAP-ABOVE-ONE-2026-10-01`,
  `T-ADM-CM-X86-TAIL-NEGATIVE-THRESHOLD-SHIFT-2026-10-01`,
  `T-ADM-CM-NEGATIVE-THRESHOLD-SHIFT-2026-10-01` (PR #1662).
