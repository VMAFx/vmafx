<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1033: CPU-side scoring NaN/UB guards across PSNR/SSIM/MS-SSIM/ADM/CAMBI/MOTION

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `correctness`, `cpu`, `psnr`, `ssim`, `adm`, `cambi`, `motion`

## Context

A round-6 audit of CPU-path scoring edge cases identified thirteen distinct
bugs that produce NaN, -Inf, or out-of-bounds memory writes on flat/black
frames or synthetic inputs. The affected metrics are APSNR (integer_psnr.c),
MS-SSIM (ms_ssim.c), float SSIM/MS-SSIM (float_ssim.c / float_ms_ssim.c),
IQA SSIM tools (iqa/ssim_tools.c), ADM (adm.c / float_adm.c / integer_adm.c),
MOTION (motion.c), and CAMBI (cambi.c). Each bug is exploitable with legal but
unusual inputs (identical reference and distortion frames, ultra-wide images,
low luminance threshold, OOM, or distorted synthetic test vectors) and causes
incorrect aggregate scores or process crashes.

Per NASA/JPL Power of 10 Rule 1 (restrict all code to the simplest control
flow) and SEI CERT C INT32-C (ensure no signed-integer overflow), the root
causes are: unchecked `log10(0)`, `pow(negative, fractional)`, signed
integer overflow in a multiply before a cast, an `assert()` reachable in
production, uninitialized output pointers on a common branch, a harmonic-mean
0/0 case, a wrong stride used in a bilinear downscale, missing OOM guards, and
a `uint16_t` subtraction that wraps when luminance threshold is low.

## Decision

Apply surgical fixes to each affected TU:

1. **integer_psnr.c APSNR flush**: bound the loop to `enable_chroma ? 3 : 1`
   planes; guard `sse[i] == 0` by emitting `psnr_max[i]` directly; remove the
   erroneous `* 2` inflation in the `max_apsnr` cap formula.
2. **ms_ssim.c pow(negative, frac)**: wrap `l` and `c` in `fabs()` in the
   main accumulation loop (line ~269), mirroring the existing `fabs()` guard
   already present for `s` in `ms_ssim_reduce_fn`.
3. **ms_ssim.c size overflow**: change `(double)(w * h)` to
   `(double)w * (double)h` to avoid signed int32 overflow for widths > 46340.
4. **float_ssim.c / float_ms_ssim.c convert_to_db**: add `if (score >= 1.0)
   return max_db;` before `log10(1 - score)` to prevent log of zero or
   negative.
5. **iqa/ssim_tools.c assert(!args)**: replace the `assert` with the existing
   downstream runtime guard (`if (args && !mr) return INFINITY`); remove the
   now-dead `<assert.h>` include.
6. **adm.c / integer_adm.c uninitialized *score_aim**: add `*score_aim = 1.0f`
   in the `den == 0` branch, which previously left the output pointer
   uninitialized.
7. **float_adm.c harmonic mean NaN**: guard `score + score_aim == 0.0` and
   return `0.0` rather than emitting `0/0` NaN through `MAX()`.
8. **float_adm.c adm_skip_scale0 sentinel**: mirror the `float_vif.c:296`
   pattern and emit an explicit `0.0` when `adm_skip_scale0` is set, rather
   than relying on the `0 / 1e-10` sentinel arithmetic.
9. **motion.c bilinear wrong stride**: pass the caller-supplied `img1_stride` /
   `img2_stride` to `motion_scale_bilinear` instead of recomputing
   `ALIGN_CEIL(width*sizeof(float))/sizeof(float)`, which does not account for
   any padding in the original allocation.
10. **motion.c aligned_malloc NULL**: add NULL guards after both `aligned_malloc`
    calls; on OOM return the already-computed `motion_scale0` gracefully.
11. **cambi.c v_band_size uint16_t overflow**: compute the band size in signed
    int; return `-EINVAL` with a diagnostic when the result is non-positive.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Clamp NaN/Inf after the fact (post-process all scores) | Single site | Masks bugs, hides root cause, all metrics affected | Does not fix root cause; NASA/JPL R1 prohibits blind workarounds |
| Leave assert in ssim_tools.c | Documents limitation | Aborts process in production on valid non-default args | Crash is worse than returning INFINITY |
| Use `UINT16_MAX` as cambi v_band_size cap | Simple | Silently over-allocates, writing OOB | Fail-fast with -EINVAL is safer |

## Consequences

- **Positive**: identical-frame (flat/black) inputs now produce valid clamped
  PSNR/SSIM/MS-SSIM/ADM scores; ultra-wide inputs no longer overflow
  MS-SSIM size; low-luminance CAMBI configs return -EINVAL instead of
  silently over-allocating; MOTION scale-1 reads the correct row data.
- **Negative**: CAMBI callers with very low `cambi_vis_lum_threshold` will
  receive -EINVAL instead of silently wrong scores; this is the correct
  behavior.
- **Neutral / follow-ups**: The `* 2` removal in APSNR max-cap slightly
  lowers the theoretical ceiling for APSNR aggregates; existing golden
  assertions are unaffected (they use normal non-identical frames where
  SSE > 0 and the cap is never hit).

## References

- Round-6 audit findings (task brief, 2026-06-04).
- `float_vif.c` skip_scale0 guard precedent: `core/src/feature/float_vif.c:296`.
- Related: ADR-1024 (r6 PSNR/ADM subset, already on `fix/r6-metric-scoring-guards`).
- NASA/JPL Power of 10 Rules 1, 4, 5.
- SEI CERT C: INT32-C, FLP32-C, MEM35-C.
