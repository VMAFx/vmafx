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

This convention rejects a failed *input or computation*. It does not remove
the established SSIM/MS-SSIM output sentinel from ADR-1221: when dB output is
enabled, clipping is explicitly disabled, and a finite raw score is at least
`1.0`, the reported score remains positive infinity. A NaN raw score, an
invalid ceiling, or a non-finite dB conversion still fails before publication.

The guard goes before the *first* append of the frame, not beside each clamp,
so a frame fails atomically rather than leaving some features published and
others missing. MS-SSIM therefore computes and validates every enabled plane
before it appends any plane, and validates the raw L/C/S atoms even when they
are not requested as output. ADM validates raw reductions before its precision
floor, since an ordered floor comparison otherwise maps negative infinity to
zero. VIF routes float and integer headline, scale, and debug results through
one complete-set emitter on every registered backend.

The remaining six sites that could not take another inline guard are split at
their natural scoring seams:

| Seam | Contract |
| --- | --- |
| `adm_score.h` | ADM/AIM and per-scale ratios plus both ADM3 formulas validate every operand and computed result, including raw aggregate values before the precision floor; they write output only on success, handle the ADM and AIM denominators independently, and define the legitimate finite flat-frame `0/0` ratio as `1.0`. |
| `nonfinite_score.h` | Float and integer VIF ratios/debug atoms, SSIM reductions/dB conversion, hidden MS-SSIM L/C/S atoms, and multi-value collector emission validate the complete score set before the first write; CPU, CUDA, HIP, SYCL and Metal hosts use the same ordering and failure contract. The ADR-1221 unclipped perfect-score infinity is the sole intentional non-finite output. |
| `ssimulacra2_score.h` | Edge-difference splitting preserves non-finite evidence and the final polynomial never maps it to `100.0`; scalar, SIMD, CUDA, HIP, SYCL and Metal hosts use the same helper. |
| `transnet_v2_score.h` | Logit-to-probability/flag conversion validates before writing either output. |
| `predict.c::predict_validate_finite` and `piecewise_linear_mapping` | Reject raw, polynomial and piecewise non-finite predictions before the old `0.0` default or collector publication; the production diagnostic names the frame and value exactly once. |

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
  hosts for VIF, ADM, SSIM, MS-SSIM and SSIMULACRA2, rather than drifting by
  backend.
- **Neutral for SSIM semantics**: a finite perfect SSIM/MS-SSIM raw score still
  reports positive infinity when the caller explicitly selects unclipped dB
  output, as specified by ADR-1221.
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
- [ADR-1221](1221-gpu-ms-ssim-db-ceiling.md) — the intentional unclipped
  perfect-score positive-infinity representation.
- `core/src/feature/brisque.c` and `core/src/feature/y_funque_plus.c` — the
  convention this follows.
- Issue #1526 — the twelve-site inventory and closure target.
- [Non-finite score laundering research digest](../research/2079-nonfinite-score-laundering-2026-09-23.md).
