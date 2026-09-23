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
| `integer_adm.c:2269` | `adm_min_val` |

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
others missing.

This ADR covers the four sites landed here. Five more are held, not abandoned,
and tracked in Issue #1526:

| File | Why it is held |
| --- | --- |
| `ssimulacra2.c` | the guard pushes `extract` past the 60-line `readability-function-size` limit, and the file has a second laundering site the first pass missed |
| `float_adm.c` | same function-size regression against a required ratchet |
| `transnet_v2.c` | same, in `transnet_v2_extract` |
| `adm.c` | the site is inside `compute_adm`, which unwinds through a single `fail:` label; a bare early return would leak |
| `predict.c` | the proposed guard rested on a premise that does not hold for any shipped model |

Each needs a helper extracted at the function's natural seam — the same
refactor ADR-1301 applied twice — rather than a looser guard.

## Alternatives considered

| Option | Why not |
| --- | --- |
| **Guard, warn, fail the frame** (chosen) | Already the convention in three extractors; a caller can tell a failure from a measurement. |
| Publish the NaN unclamped | Every consumer — collector, pooling, JSON writer — would have to learn to handle it, and a NaN in the pooled VMAF score is a worse surface than a failed frame. |
| Clamp non-finite to the *worst* score | Still a lie, just a pessimistic one, and for VIF the worst value is already what a NaN becomes. |
| Fix all twelve in one change | Five need a function split each, against a required ratchet, in Netflix golden-data paths. Landing the four that need no refactor keeps the reviewable part reviewable. |
| Leave them, since no fixture produces a NaN today | The golden fixtures not reaching the path is exactly why this survived: it is invisible until it matters. |

## Consequences

- **Positive**: a non-finite `vif`, `adm`, `ssim` or `ms_ssim` score is
  reported as a failure instead of as a perfect or worst-case measurement.
- **Positive**: the finiteness convention is now the same in six extractors.
- **Neutral for scores**: verified, not assumed. The Netflix golden gate is
  `271 passed, 12 skipped` both before and after.
- **Negative**: a frame that previously produced a wrong-but-finite score now
  fails, which is the intent but is a behaviour change for any caller
  unknowingly consuming one.
- **Negative**: five sites still launder a NaN until #1526 closes.

## References

- req: "lol fix? wtf" — the user, on being told the masking had been found and
  filed rather than fixed.
- [ADR-1301](1301-speed-nonfinite-score-fails-frame.md) — the same defect in
  SpEED, and the shared helper it introduced.
- `core/src/feature/brisque.c` and `core/src/feature/y_funque_plus.c` — the
  convention this follows.
- Issue #1526 — the five held sites.
