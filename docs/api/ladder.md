# `vmaftune.ladder` — Python API reference

The `vmaftune.ladder` module provides the core building blocks for constructing
per-title bitrate ladders. It is consumed by the `vmaf-tune ladder` CLI subcommand
and can be used directly in custom Python pipelines.

See [usage/vmaf-tune-ladder.md](../usage/vmaf-tune-ladder.md) for the CLI
reference and worked examples. See [usage/vmaf-tune-bitrate-ladder.md](../usage/vmaf-tune-bitrate-ladder.md)
for the grid-based variant.

## Data types

### `LadderPoint`

```python
@dataclass
class LadderPoint:
    width: int
    height: int
    bitrate_kbps: float
    vmaf: float
    crf: int
```

A single (resolution, bitrate, VMAF, CRF) observation from an encoding run.
`convex_hull` and `select_knees` operate on sequences of `LadderPoint`.

### `UncertaintyLadderPoint`

```python
@dataclass
class UncertaintyLadderPoint(LadderPoint):
    vmaf_low: float   # lower bound of the conformal prediction interval
    vmaf_high: float  # upper bound of the conformal prediction interval
```

Extension of `LadderPoint` that carries a conformal-prediction interval
`(vmaf_low, vmaf_high)`. Used by the uncertainty-aware recipe
([ADR-0393](../adr/0393-fr-regressor-v2-probabilistic.md)).
The interval width `vmaf_high - vmaf_low` is the predictor's local
confidence: small = tight, large = wide.

`as_ladder_point()` strips the interval fields and returns a plain
`LadderPoint` for use in `convex_hull` / `select_knees`.

## Functions

### `convex_hull`

```python
def convex_hull(
    points: Sequence[LadderPoint],
) -> list[LadderPoint]:
```

Computes the Pareto-efficient upper convex hull of the input observations in
the (bitrate, VMAF) plane. A point is on the hull if and only if no other
point has both higher VMAF and lower bitrate.

The result is sorted in ascending bitrate order. `select_knees` must receive
hull output, not raw observations.

**Invariant preserved by the uncertainty-aware recipe:** the hull is
recomputed on `apply_uncertainty_recipe`'s output before `select_knees` is
called, so the Pareto-frontier property holds over the augmented rung set.

### `select_knees`

```python
def select_knees(
    hull: Sequence[LadderPoint],
    n: int,
    spacing: Literal["uniform", "log_bitrate"] = "log_bitrate",
) -> list[LadderPoint]:
```

Selects `n` rungs from the convex hull. `spacing` controls how the rungs are
distributed across the bitrate axis:

| `spacing` | Behaviour |
| --- | --- |
| `"log_bitrate"` | Divides the log-bitrate range into `n - 1` equal steps and picks the hull point nearest to each step boundary. Matches HLS authoring convention and human perception of "equal quality steps." **Default.** |
| `"uniform"` | Divides the linear bitrate range into `n - 1` equal steps. Produces denser coverage at low bitrate; suitable for VoD at fixed bandwidth caps. |

Returns exactly `n` rungs, or fewer if the hull contains fewer than `n` points.
The first and last hull points are always included (min-bitrate and max-bitrate
anchors).

### `emit_manifest`

```python
def emit_manifest(
    rungs: Sequence[LadderPoint],
    format: Literal["hls", "dash", "json"] = "json",
    target_duration_s: float = 6.0,
) -> str:
```

Serialises the selected rungs to a manifest string.

| `format` | Output |
| --- | --- |
| `"hls"` | `#EXTM3U` / `#EXT-X-STREAM-INF:` playlist pointing at per-rung `.m3u8` renditions. |
| `"dash"` | MPD XML with one `AdaptationSet` and one `Representation` per rung. |
| `"json"` | JSON array `[{width, height, bitrate_kbps, vmaf, crf}, ...]` (default). |

`target_duration_s` appears in the HLS `#EXT-X-TARGETDURATION` header.

### `apply_uncertainty_recipe`

```python
def apply_uncertainty_recipe(
    points: Sequence[UncertaintyLadderPoint],
    thresholds: ConfidenceThresholds | None = None,
) -> list[UncertaintyLadderPoint]:
```

Applies the two-stage uncertainty-aware recipe to the input rungs:

1. **Prune** redundant rungs whose conformal intervals overlap more than
   `thresholds.rung_overlap_threshold`.
2. **Insert** synthetic mid-rungs between adjacent pairs whose averaged
   interval width exceeds `thresholds.wide_interval_min_width`.

Both transforms run **after** `convex_hull` and **before** `select_knees`.
The caller is responsible for calling `convex_hull` on the output before
passing it to `select_knees`.

`thresholds` defaults to `ConfidenceThresholds()` (Research-0067 floor:
`tight_max = 2.0`, `wide_min = 5.0` VMAF) when `None`.

See [usage/vmaf-tune-ladder.md](../usage/vmaf-tune-ladder.md) for a complete
worked example.

## Typical call sequence

```python
from vmaftune.ladder import (
    LadderPoint,
    convex_hull,
    select_knees,
    emit_manifest,
)

# Observations from your encoding sampler
raw: list[LadderPoint] = run_sampler(source, encoder, crf_sweep)

hull = convex_hull(raw)
rungs = select_knees(hull, n=5, spacing="log_bitrate")
print(emit_manifest(rungs, format="hls"))
```

## See also

- [usage/vmaf-tune-ladder.md](../usage/vmaf-tune-ladder.md) — CLI flag reference
  and the uncertainty-aware extension
- [usage/vmaf-tune-recommend.md](../usage/vmaf-tune-recommend.md) — per-clip
  CRF-search consumer
- [ai/conformal-vqa.md](../ai/conformal-vqa.md) — the conformal-prediction
  surface that provides `vmaf_low` / `vmaf_high` intervals
- [usage/vmaf-tune.md](../usage/vmaf-tune.md) — vmaf-tune overview
