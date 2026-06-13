<!-- markdownlint-disable MD060 -->
# `vmaf-tune ladder` — uncertainty-aware ABR ladder construction

`vmaf-tune ladder` builds a per-title bitrate ladder by sampling the
(resolution × target-VMAF) plane, computing the convex hull of the
resulting (bitrate, VMAF) cloud, picking `--quality-tiers` knees along
the hull, and emitting an HLS / DASH / JSON manifest. The base
behaviour is documented in
[`vmaf-tune.md`](vmaf-tune.md#per-title-ladder-phase-e) and is unchanged when
the uncertainty flags are omitted.

This page covers the **uncertainty-aware extension**
([ADR-0393](../adr/0393-fr-regressor-v2-probabilistic.md), shipped on
top of the conformal-VQA prediction surface in PR #488). The
extension is wired through both the library API and the CLI. The
default sampler preserves `vmaf_interval` blocks from corpus rows; when
those intervals are present, `--with-uncertainty` applies the same
prune/insert recipe before knee selection. Point-only rows use the
active `wide_interval_min_width` as a conservative centred fallback,
so the recipe can still probe uncertain bitrate gaps.

## Why uncertainty-aware

A point-estimate ladder treats every rung's VMAF as exact. In
practice the predictor's intervals carry information that bears
directly on rung selection:

- **Adjacent rungs whose intervals overlap a lot** — the predictor
  cannot statistically distinguish them. Shipping both adds no
  quality information; ship the higher-quality rung and drop the
  lower-bitrate one.
- **Adjacent rungs whose averaged interval width is in the
  WIDE band** — the predictor cannot localise even the rung. Insert
  a synthetic mid-rung; this is the highest-information-per-encode
  use of the budget because the operator empirically does not know
  which of "ship A" / "ship B" / "ship a hypothetical mid-rung" is
  best.

Both transforms run **after** [`convex_hull`](../api/ladder.md) and
**before** [`select_knees`](../api/ladder.md), so the Pareto-frontier
invariant is preserved.

## Flag surface

| Flag | Default | Purpose |
|---|---|---|
| `--with-uncertainty` | off | Apply uncertainty-aware rung pruning/insertion. Sampled `vmaf_interval` payloads win; point-only rows use a conservative centred interval based on `wide_interval_min_width`. |
| `--uncertainty-sidecar PATH` | none | Calibration sidecar JSON (same schema as `recommend --uncertainty-sidecar`). Falls back to the documented Research-0067 floor. |
| `--rung-overlap-threshold F` | 0.5 | Overlap fraction above which two adjacent rungs are treated as indistinguishable and the lower-bitrate one is dropped. |
| `--framerate F` | `24.0` | Source frame rate fed to the default corpus sampler. Set to match the real source — the legacy hardcoded `24.0` produced incorrect bitrate math on non-24 fps inputs (ADR-0497). |
| `--duration S` | `1.0` | Analysed window length in seconds. Drives `bitrate_kbps = encode_size / duration` AND (ADR-0506) bounds the ffmpeg encode pipe and the reference decode to the first `S` seconds via input-side `-t`. Set this to a short value (e.g. `--duration 10`) when smoke-testing the ladder against a long source — pre-ADR-0506 the flag was metadata-only and a 10-second probe against a 9-minute container re-encoded the full source at every CRF in the sweep. |
| `--pix-fmt FMT` | `yuv420p` | Source pixel format fed to the default corpus sampler (e.g. `yuv422p`, `yuv420p10le`). Symmetric with `compare` / `tune-per-shot` (ADR-0497). |
| `--crf-sweep CSV` | `18,23,28,33,38` | Comma-separated CRF list overriding the canonical 5-point default sweep. Useful for smoke runs that only need to verify the ladder plumbing (e.g. `--crf-sweep 23,28`, ADR-0497). |

The threshold defaults are documented in
[`vmaftune.uncertainty.ConfidenceThresholds`](../ai/conformal-vqa.md):
`tight_interval_max_width = 2.0` and `wide_interval_min_width = 5.0`
VMAF, sourced from
[Research-0067](../research/0067-vmaf-tune-phase-f-feasibility-2026-05-08.md)
and shared with the auto-driver F.3 loader. The
`DEFAULT_RUNG_OVERLAP_THRESHOLD = 0.5` is documented in the same
research note.

## The two transforms

### `prune_redundant_rungs_by_uncertainty`

Walks the input rungs in ascending-bitrate order. For each adjacent
pair `(prev, cur)`, computes the overlap of their conformal intervals
divided by the wider interval's width. When the overlap exceeds
`overlap_threshold`, drops `prev` — the lower-bitrate rung.

The first and last rungs are always retained so the ladder's bitrate
range is preserved; `select_knees` later picks interior rungs from
whatever survives.

### `insert_extra_rungs_in_high_uncertainty_regions`

Walks the rungs in ascending-bitrate order. For each adjacent pair
`(a, b)`, computes the pair-averaged interval width. When the
averaged width is `>= wide_interval_min_width`, inserts a synthetic
rung at:

- **Bitrate** — geometric midpoint of `(a.bitrate, b.bitrate)`. The
  geometric mean matches the log-bitrate spacing convention used by
  HLS authoring (see
  [`select_knees`](../api/ladder.md) `spacing="log_bitrate"`).
- **VMAF** — arithmetic midpoint of `(a.vmaf, b.vmaf)`.
- **Interval** — union of the parent intervals
  (`min(a.low, b.low), max(a.high, b.high)`). Conservative on
  purpose: subsequent encodes refine it.
- **CRF** — rounded average of the parent CRFs.
- **Resolution** — inherited from the higher-quality parent.

### `apply_uncertainty_recipe`

Composes the two transforms in their canonical order: prune first
(so the inserted mid-rungs aren't immediately re-pruned against
their parents), then insert.

## Worked example — library API

```python
from vmaftune.ladder import (
    UncertaintyLadderPoint,
    apply_uncertainty_recipe,
    convex_hull,
    select_knees,
    emit_manifest,
)
from vmaftune.uncertainty import (
    ConfidenceThresholds,
    load_confidence_thresholds,
)

# Sampler emits points with conformal intervals attached.
sampled = [
    UncertaintyLadderPoint(
        width=1920, height=1080, bitrate_kbps=8000.0,
        vmaf=95.5, crf=20, vmaf_low=92.5, vmaf_high=98.5,  # WIDE
    ),
    UncertaintyLadderPoint(
        width=1280, height=720, bitrate_kbps=2500.0,
        vmaf=91.0, crf=24, vmaf_low=88.0, vmaf_high=94.0,  # WIDE
    ),
    UncertaintyLadderPoint(
        width=854, height=480, bitrate_kbps=1200.0,
        vmaf=85.0, crf=27, vmaf_low=84.5, vmaf_high=85.5,  # tight
    ),
]

thresholds = load_confidence_thresholds("calibration.json")
augmented = apply_uncertainty_recipe(sampled, thresholds=thresholds)
# `augmented` may contain synthetic mid-rungs in any wide-interval gap.

hull = convex_hull([p.as_ladder_point() for p in augmented])
rungs = select_knees(hull, n=5, spacing="log_bitrate")
print(emit_manifest(rungs, format="hls"))
```

Reading the above: the (1080p, 8000 kbps) rung and the (720p,
2500 kbps) rung have intervals of width 6.0 each — averaged width 6.0
is `>= wide_min = 5.0`, so the recipe inserts a synthetic
~4470 kbps / 93.25 VMAF rung between them. The (480p, 1200 kbps)
tight rung is left alone.

## Decision rules

| Transform | Condition | Action |
|---|---|---|
| Prune | overlap fraction > `rung_overlap_threshold` | Drop the lower-bitrate rung. |
| Insert | pair-averaged width >= `wide_interval_min_width` | Add a synthetic mid-rung. |

When the sampler ships plain `LadderPoint` rungs without intervals,
`--with-uncertainty` widens each point to a centred fallback interval
whose width is the active `wide_interval_min_width`. That keeps
point-only corpora conservative: pruning still has real overlap data
to evaluate, and wide adjacent gaps can receive a synthetic midpoint
rung.

## Worked example — CLI

```text
$ vmaf-tune ladder --src trailer.mp4 --encoder libx264 \
    --resolutions 1920x1080,1280x720,854x480 \
    --target-vmafs 95,90,85 --quality-tiers 5 \
    --with-uncertainty --uncertainty-sidecar calibration.json
#EXTM3U
#EXT-X-VERSION:6
#EXT-X-STREAM-INF:BANDWIDTH=1200000,RESOLUTION=854x480,CODECS="avc1.640028"
rendition_854x480_1200k.m3u8
...
```

If the sampled corpus rows contain `vmaf_interval` objects, the CLI
attaches those intervals to the post-hull rungs, calls
`apply_uncertainty_recipe()`, and selects knees from the adjusted rung
set. If the rows are point-only, `--with-uncertainty` loads the
threshold sidecar and uses its `wide_interval_min_width` as the
fallback interval width before running the same recipe.

## What this does NOT change

Per the [`feedback_no_test_weakening`](../../CLAUDE.md) project rule,
the uncertainty-aware recipe affects **which rungs the ladder
builder evaluates**. It does **not** change:

- The Netflix golden-data assertions
  ([§8 of CLAUDE.md](../../CLAUDE.md#8-netflix-golden-data-gate-do-not-modify)).
- The convex-hull invariant
  ([`convex_hull`](../api/ladder.md) is unchanged).
- The knee-selection invariant
  ([`select_knees`](../api/ladder.md) is unchanged).
- The HLS / DASH / JSON manifest schema
  ([`emit_manifest`](../api/ladder.md) is unchanged).

## See also

- [`docs/ai/conformal-vqa.md`](../ai/conformal-vqa.md) — the
  underlying conformal-prediction surface (PR #488 / ADR-0279).
- [`docs/usage/vmaf-tune-recommend.md`](vmaf-tune-recommend.md) —
  the per-clip CRF-search consumer of the same intervals.
- [`docs/research/0067-vmaf-tune-phase-f-feasibility-2026-05-08.md`](../research/0067-vmaf-tune-phase-f-feasibility-2026-05-08.md)
  — provenance of the threshold defaults.
