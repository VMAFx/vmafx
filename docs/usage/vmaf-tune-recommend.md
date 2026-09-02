<!-- markdownlint-disable MD013 MD060 -->
# `vmaf-tune recommend` — uncertainty-aware CRF target search

`vmaf-tune recommend` searches a corpus (or builds one on the fly) for
the smallest CRF whose VMAF clears `--target-vmaf`. Without
uncertainty flags, behaviour is the point-estimate predicate
documented in [Research-0061](../research/0061-vmaf-tune-capability-audit.md)
Bucket 5: scan every row, return the smallest CRF whose
`vmaf_score >= target`.

This page covers the **uncertainty-aware extension**
([ADR-0393](../adr/0393-fr-regressor-v2-probabilistic.md), shipped on
top of the conformal-VQA prediction surface in PR #488). The base
behaviour is documented in
[`vmaf-tune.md`](vmaf-tune.md#recommend-subcommand-target-vmaf-target-bitrate) and is unchanged
when the uncertainty flags are omitted.

## Why uncertainty-aware

The point-estimate recipe treats every row's VMAF as exact. In
practice the predictor's residuals carry a distribution; the
[conformal-prediction surface](../ai/conformal-vqa.md) wraps each
prediction in a `(low, high)` interval whose width is the
predictor's local confidence:

- **Tight interval** — the predictor is confident; the lower bound
  is a faithful proxy for the truth and the search can short-circuit
  the moment a row whose `low >= target` is observed.
- **Wide interval** — the predictor is uncertain; the search refuses
  to short-circuit on any single row and falls back to a full scan
  with the result tagged `(UNCERTAIN)`.
- **Middle band** — defer to the existing point-estimate predicate
  exactly. This preserves pre-uncertainty behaviour for callers who
  upgrade the binary but keep their corpus uncalibrated.

The thresholds carving these three bands are documented in
[`vmaftune.uncertainty.ConfidenceThresholds`](../ai/conformal-vqa.md):
defaults are `tight_interval_max_width = 2.0` and
`wide_interval_min_width = 5.0` VMAF, sourced from
[Research-0067](../research/0067-vmaf-tune-phase-f-feasibility-2026-05-08.md)
and mirrored byte-for-byte from the auto-driver F.3 loader.

## Flag surface

| Flag | Default | Purpose |
|---|---|---|
| `--with-uncertainty` | off | Switches the recommend pipeline to the interval-aware predicate. |
| `--uncertainty-sidecar PATH` | none | Calibration sidecar JSON; falls back to the documented Research-0067 floor (tight=2.0, wide=5.0 VMAF) when absent. |

The sidecar schema matches the auto-driver F.3 loader byte-for-byte
so a single calibration sidecar drives `auto`, `recommend`, and
`ladder` without divergence:

```json
{
  "tight_interval_max_width": 1.6,
  "wide_interval_min_width": 4.2
}
```

Extra keys are ignored so the loader survives schema growth.

## Decision rules

| Band | Condition | Action |
|---|---|---|
| TIGHT | `width <= tight_max` AND `low >= target` | Short-circuit at the first qualifying row. `O(k)` scan. |
| WIDE | At least one row has `width >= wide_min` | Force full scan; flag result `(UNCERTAIN)`. |
| MIDDLE / NaN | otherwise | Defer to the native point-estimate predicate. |

When every visited row's interval lies strictly below the target
(`high < target` for all rows), the result is tagged `UNMET,
interval-excluded` and surfaces the highest-VMAF best-effort row.

## Worked example

```text
$ vmaf-tune recommend --source ref.yuv --width 1920 --height 1080 \
    --encoder libx264 --preset medium --target-vmaf 93.0 \
    --coarse-to-fine --with-uncertainty \
    --uncertainty-sidecar calibration.json \
    --output corpus.jsonl

vmaf-tune: scoring backend = cpu
src=ref.yuv preset=medium crf=20 vmaf=94.250 \
    decision=tight visited=2/15 \
    predicate=target_vmaf>=93.0 (TIGHT, low=93.420)
```

Reading the output:

- `decision=tight` — the conformal interval at CRF 20 has
  `width=0.6 <= tight_max=2.0` and `low=93.42 >= target=93.0`, so the
  search short-circuited.
- `visited=2/15` — the search examined 2 rows out of the 15 the
  coarse-to-fine sweep would have produced. The remaining 13 encodes
  were skipped, saving wall-clock time.
- `predicate=...(TIGHT, low=93.420)` — the predicate that fired,
  with the lower bound that promoted the row.

If the calibration sidecar were missing, the same call would emit a
WARNING and fall back to the Research-0067 defaults; the search
would still run, just with the more conservative gates. If the
predictor's intervals were all `width >= 5.0`, the output would
instead read:

```text
src=ref.yuv preset=medium crf=20 vmaf=94.250 \
    decision=wide visited=15/15 \
    predicate=target_vmaf>=93.0 (UNCERTAIN)
```

## Library entry point

```python
from vmaftune.recommend import (
    UncertaintyAwareRequest,
    pick_target_vmaf_with_uncertainty,
)
from vmaftune.uncertainty import (
    ConfidenceDecision,
    ConfidenceThresholds,
    load_confidence_thresholds,
)

thresholds = load_confidence_thresholds("calibration.json")
req = UncertaintyAwareRequest(
    target_vmaf=93.0,
    thresholds=thresholds,
    encoder="libx264",
    preset="medium",
)
result = pick_target_vmaf_with_uncertainty(rows, req)
assert result.decision is not ConfidenceDecision.WIDE  # confident pick
```

Per-call interval overrides are supported via the
`sample_uncertainty` keyword — useful when the deep-ensemble +
conformal pipeline produces intervals out-of-band:

```python
overrides = {
    20: (94.0, 93.5, 94.5),  # (point, low, high) at CRF 20
    23: (91.0, 90.0, 92.0),
}
req = UncertaintyAwareRequest(
    target_vmaf=93.0,
    sample_uncertainty=overrides,
)
```

## What this does NOT change

Per the [`feedback_no_test_weakening`](../../CLAUDE.md) project rule,
the uncertainty-aware recipe affects **search cost** and **which
qualifying row gets picked from an equivalence class**. It does
**not** change:

- The Netflix golden-data assertions
  ([§8 of CLAUDE.md](../../CLAUDE.md#8-netflix-golden-data-gate-do-not-modify)).
- The production-flip gate in `predictor_validate.py`. That gate
  decides which encodes get shipped, not which encodes get probed.
- The point estimate itself —
  [`Predictor.predict_vmaf`](../api/predictor.md) returns the same
  scalar with or without uncertainty wiring.

## See also

- [`docs/ai/conformal-vqa.md`](../ai/conformal-vqa.md) — the
  underlying conformal-prediction surface (PR #488 / ADR-0279).
- [`docs/usage/vmaf-tune-ladder.md`](vmaf-tune-ladder.md) — the
  ABR-ladder consumer of the same intervals.
- [`docs/research/0067-vmaf-tune-phase-f-feasibility-2026-05-08.md`](../research/0067-vmaf-tune-phase-f-feasibility-2026-05-08.md)
  — provenance of the threshold defaults.
