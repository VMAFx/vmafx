# ADR-1302: A non-finite feature score fails the frame, everywhere

- **Status**: Accepted
- **Date**: 2026-09-23
- **Deciders**: Lusoris
- **Tags**: `metrics`, `correctness`, `fork-local`

## Context

[ADR-1301](1301-speed-nonfinite-score-fails-frame.md) fixed this for SpEED: a
score bounded by a less-than comparison publishes a NaN as the bound, because
every comparison against NaN is false. A sweep of all 466
`vmaf_feature_collector_append*` call sites across 108 files in `core/src`
found the same shape at twelve more places.

Several are worse than SpEED's, and worse in a specific way. SpEED published a
NaN as `1000.0` — an implausible number that might draw the eye. These publish
a NaN as the **best possible score**:

| Site | A NaN is published as |
| --- | --- |
| `ssimulacra2.c:916` | `100.0` — SSIMULACRA 2's perfect score |
| `adm.c:379` | `1.0f` — a perfect AIM score |
| `float_ssim.c` `convert_to_db()` | `max_db` — perfect similarity |
| `float_ms_ssim.c` `convert_to_db()` | `max_db` — the same, copied |
| `float_vif.c:341,345,349` | `vif_scaleN_min_val`, 0.0 by default — the *worst* VIF value |
| `float_adm.c:479,483` | `adm_min_val` |
| `integer_adm.c:2269` | `adm_min_val` |
| `transnet_v2.c:332` | boundary flag `0.0` |
| `predict.c:147` | aggregate prediction `0.0` |

The SSIM one is the sharpest illustration, because its guard was written *for
this situation* and still misses it:

```c
/* score >= 1.0 makes log10(1-score) undefined (log10 of zero or negative)
   yielding -Inf / NaN.  Return max_db directly for perfect similarity.  */
if (score >= 1.0)
    return max_db;
return MIN(-10. * log10(1.0 - score), max_db);
```

`NaN >= 1.0` is false, so a NaN falls past the guard into `MIN(NaN, max_db)`,
which returns `max_db` — the exact value the guard reserves for a perfect
result.

`adm`, `vif` and `ssim` are core VMAF sub-features. A NaN in any of them
surfaces as a finite, in-range, plausible number, so nothing downstream can
distinguish a measurement from a non-measurement.

## Decision

Extend the existing convention — `brisque.c`, `y_funque_plus.c` and now
`speed_internal_clamp_score()` — to the rest of the metric engine: guard with
`isfinite` **before** the comparison that would launder the value, warn naming
the extractor, the frame and the value, and return `-EINVAL`.

The guard goes before the *first* append of the frame, not beside each clamp,
so a frame fails atomically rather than leaving some features published and
others missing. MS-SSIM therefore computes and validates every enabled plane
before it appends any plane.

The remaining six sites that could not take another inline guard are split at
their natural scoring seams:

| Seam | Contract |
| --- | --- |
| `adm_score.h` | ADM/AIM ratios and both ADM3 formulas validate every operand and computed result, write output only on success, and preserve the legitimate finite flat-frame `1.0` result. |
| `ssimulacra2_score.h` | Edge-difference splitting preserves non-finite evidence and the final polynomial never maps it to `100.0`; scalar, SIMD, CUDA, HIP, SYCL and Metal hosts use the same helper. |
| `transnet_v2_score.h` | Logit-to-probability/flag conversion validates before writing either output. |
| `predict.c::piecewise_linear_mapping` | Rejects a non-finite input before assigning the old `0.0` default. |

All helpers publish output atomically: on `-EINVAL`, caller-owned output values
remain unchanged. This gives the regression tests a deterministic injection
point without changing Netflix golden fixtures or assertions.

## Alternatives considered

| Option | Why not |
| --- | --- |
| **Guard, warn, fail the frame** (chosen) | Already the convention in three extractors; a caller can tell a failure from a measurement. |
| Publish the NaN unclamped | Every consumer — collector, pooling, JSON writer — would have to learn to handle it, and a NaN in the pooled VMAF score is a worse surface than a failed frame. |
| Clamp non-finite to the *worst* score | Still a lie, just a pessimistic one, and for VIF the worst value is already what a NaN becomes. |
| Leave the five helper-sized paths for another PR | Rejected after the first bounded commit: it would leave Issue #1526 knowingly open and preserve the most flattering failures. The helper seams keep the completed change reviewable. |
| Leave them, since no fixture produces a NaN today | The golden fixtures not reaching the path is exactly why this survived: it is invisible until it matters. |

## Consequences

- **Positive**: every laundering site found by the 466-call-site sweep now
  reports failure instead of a perfect, worst-case or otherwise plausible
  measurement.
- **Positive**: the finiteness convention is shared by the CPU, SIMD and GPU
  SSIMULACRA2 hosts, rather than drifting by backend.
- **Neutral for scores**: verified, not assumed. The Netflix golden gate is
  `271 passed, 12 skipped` both before and after.
- **Negative**: a frame that previously produced a wrong-but-finite score now
  fails, which is the intent but is a behaviour change for any caller
  unknowingly consuming one.
- **Negative**: callers that unknowingly relied on a fabricated finite value
  now receive `-EINVAL` and must handle the failed frame.

## References

- req: "lol fix? wtf" — the user, on being told the masking had been found and
  filed rather than fixed.
- [ADR-1301](1301-speed-nonfinite-score-fails-frame.md) — the same defect in
  SpEED, and the shared helper it introduced.
- `core/src/feature/brisque.c` and `core/src/feature/y_funque_plus.c` — the
  convention this follows.
- Issue #1526 — the twelve-site inventory and closure target.
- [Non-finite score laundering research digest](../research/nonfinite-score-laundering-2026-09-23.md).
