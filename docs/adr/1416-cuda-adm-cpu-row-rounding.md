<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1416: `adm_cuda` takes its CSF weights, its rounding shifts and its score conclusion from the CPU's routines and folds the denominator once per row

- **Status**: Accepted
- **Date**: 2026-10-01
- **Deciders**: lusoris
- **Tags**: `cuda`, `gpu-parity`, `numerics`, `adm`, `testing`, `ci`, `rc3`, `fork-local`

## Context

The integer ADM pipeline is integer arithmetic up to its last step, so a
twin should have the CPU's accumulators and, from them, the CPU's scores.
`adm_cuda` did not: on the Netflix 576x324 pair 67 of 336 outputs were
identical and `integer_adm_scale1` was up to 1.8e-7 off, at 3840x2160 up to
2.1e-7 ([Research-1403](../research/1403-cuda-strict-fp-every-kernel.md)).
Printing the raw accumulators and the CSF weights of both sides for one frame
showed three places where the twin carried its own version of something the
CPU computes ([Research-1416](../research/1416-cuda-adm-cpu-row-rounding.md)):

1. **The CSF weights.** `integer_adm_cuda.c` had a copy of
   `dwt_quant_step()` and `adm_csf_factors()`. The CPU's version
   (`integer_adm_kernels.h`) evaluates the exponent `k * temp * temp` in
   `double` since #552; the copy still evaluated it in `float`, as upstream
   Netflix does. The weights came out 1 to 3 units in the last place apart.
   Scale 0 converts them to 16-bit fixed point, where both round to the same
   integer; scales 1 to 3 convert to 32 bits, so every contrast-masking
   accumulator of those scales differed (7e-7 relative), and every
   denominator through `pow(rfactor, 3)`. This is the whole measured
   distance on the standard fixtures.
2. **The denominator fold.** The CPU adds the cube terms of a row and folds
   the row with one rounding shift (`adm_csf_den_fold()`). The kernels
   folded each warp of a row. The accumulators differed by a few units in
   1e14, which the float conversion hides on ordinary content and shows on
   frames with little reference detail (6.6e-7 on a flat 640x360 frame with
   sparse one-level dots).
3. **The shift itself.** The scale-0 kernel derived `shift_accum` as
   `ceil(log2(area) - 20)` with the device's fp32 `__log2f`; the host
   concluded with the same formula in `double`. For 81 region areas up to
   2^26 the two disagree by one, because fp32 rounds the logarithm of a
   value just above a power of two down to the integer. A 962x13542 frame
   (region 387 x 5419 = 2^21 + 1) reported `integer_adm_scale0` 0.860 where
   the CPU reports 0.979.

A fourth difference is in the options: with `adm_skip_scale0` the CPU seeds
the scale-0 denominator with `1e-10` narrowed to `float` and adds it to the
frame sum; the twin published `1e-10` as a `double` and left it out
(2.8e-13 in `adm2`).

## Decision

We will make `adm_cuda` return the CPU extractor's values bit for bit by
calling the CPU's routines for everything outside the kernels and by folding
the denominator where the CPU folds it.

**One set of host routines.** `integer_adm_cuda.c` includes
`feature/integer_adm_kernels.h`. Its copies of `dwt_quant_step()`,
`adm_csf_factors()`, `conclude_adm_cm()` and `conclude_adm_csf_den()` are
deleted. The CSF weights come from `adm_csf_factors()`; the border and the
rounding shifts of the denominator launches from `adm_csf_den_ctx_init()` /
`i4_adm_csf_den_ctx_init()`; the per-scale numerator and denominator from
`adm_cm_result()` / `i4_adm_cm_result()` / `adm_csf_den_result()` /
`i4_adm_csf_den_result()` on contexts the CPU's initialisers fill.
`adm_skip_scale0` leaves the CPU's seeds.

**One fold per row.** `adm_csf_den.cu` runs one block per row and band. The
threads add the cube terms of their columns, the block reduces them to the
row total, and thread 0 folds it once through
`adm_csf_den_round_row_total()` (`adm_cm_accumulator.h`), which the CPU's
`adm_csf_den_fold()` now calls too. The shifts are kernel arguments; the
file evaluates no logarithm.

**Gate.** `adm` gets a `cuda` entry in `EXACT_TWINS`: the CPU ↔ CUDA cell is
compared with tolerance 0 at `--precision max`.

The contrast-masking kernels (`adm_cm.cu`) are unchanged. They already fold
whole rows (ADR-1392), and their device-derived shifts are
`ceil(log2(w))` and `ceil(log2(h))` of integers, which the device evaluates
like the CPU for every extent from 1 to 131 071 (checked exhaustively).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Correct the copy of `dwt_quant_step()` in place | Two-line change | Still a second implementation; it drifted once already, when #552 changed the CPU | HISS-19 |
| Restore upstream's `float` exponent on the CPU instead | CPU and upstream Netflix agree again | Moves every CPU ADM score by up to 2e-7; a decision about the reference, not about a twin | Recorded as `T-ADM-CSF-EXPONENT-NOT-UPSTREAM-2026-10-01`; the twin follows whatever the CPU routine does |
| Keep the per-warp fold, since the scores of the standard fixtures do not show it | No kernel change | Not the CPU's accumulator; visible on low-detail frames | The contract is the CPU's bits |
| Keep the device `__log2f` shift and compute the same fp32 value on the host | No new kernel arguments | Two wrong shifts that agree; the CPU's is the `double` one | The denominator would still differ from the CPU by a factor of two on those frames |
| Sum 128 columns per thread across several blocks per row and combine them in a second kernel | Wider rows per launch | Two launches and a scratch buffer for a sum one block does | One block per row is enough: a 3840x2160 row is 1536 columns |

## Consequences

- **Positive**: measured on an RTX 4090 at `--precision max`, every output
  of every frame equals `--backend cpu`: Netflix 576x324 at 8 bits (48
  frames) and at 10, 12 and 16 bits (3 frames each), both 1920x1080
  checkerboard pairs (3 frames each) and BBB 3840x2160 (50 frames), 2 034
  of 2 034 outputs with `debug=true` (7 scores and 11 sums per frame).
  Before: 185 of 791 scores, largest difference 2.1e-7. Also identical with
  `adm_csf_mode` 1, 2 and 3, non-default `adm_p_norm`, `adm_noise_weight`,
  `adm_enhn_gain_limit`, `adm_min_val`, `adm_dlm_weight`, viewing geometry,
  `adm_skip_scale0` and `adm_skip_aim`.
- **Positive**: frames whose scale-0 border region has one of the 81 areas
  score correctly (962x13542: `integer_adm_scale0` 0.8596 before, 0.9791
  after, the CPU's value).
- **Positive**: a change to the CPU's CSF weights, borders, shifts or score
  conclusion reaches the twin without an edit.
- **Cost**: none measured. BBB 3840x2160, one twin per run,
  `(t(200) - t(2)) / 198`: 3.63 ms before, 3.66 after (nine alternating
  pairs, paired difference +0.01, quartiles -0.02 to +0.22). One more
  `adm_cuda` instance in a process that already has the frame: 2.47 ms
  before, 2.48 after (median of seven runs of nine instances against one).
- **Negative**: stored `adm_cuda` outputs change by up to 2.1e-7, and on the
  frame sizes of item 3 by much more.
- **Neutral / follow-ups**:
  - `adm_hip` folds the denominator per thread and derives the same fp32
    shift: `T-HIP-ADM-CSF-DEN-FOLD-PER-THREAD-2026-10-01`.
  - The CPU's own CSF exponent differs from upstream Netflix since #552:
    `T-ADM-CSF-EXPONENT-NOT-UPSTREAM-2026-10-01`.
  - Found while testing options: `adm_csf_mode=1` with `adm_csf_scale=1.2`
    fails every frame of the 1 px checkerboard on the CPU and on the twin
    alike (`T-ADM-AIM-BARTEN-SCALE-TERM-WRAP-2026-10-01`).

## References

- `req` (maintainer brief, 2026-10-01): "results before speed; a twin
  reproduces the CPU bit for bit, and tuning comes afterwards."
- [ADR-1403](1403-cuda-strict-fp-every-kernel.md),
  [ADR-1397](1397-psnr-hvs-twins-cpu-float-sum.md),
  [ADR-1392](1392-cuda-integer-reductions-one-atomic-per-block.md),
  [ADR-1325](1325-integer-adm-barten-fixed-point-normalization.md),
  [ADR-0214](0214-gpu-parity-ci-gate.md),
  [ADR-0024](0024-netflix-golden-preserved.md).
- [Research-1416](../research/1416-cuda-adm-cpu-row-rounding.md).
