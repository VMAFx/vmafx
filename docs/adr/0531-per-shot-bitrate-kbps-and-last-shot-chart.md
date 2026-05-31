<!-- markdownlint-disable MD060 -->
# ADR-0531: Per-shot plan emits bitrate_kbps + chart shows last shot

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `vmaf-tune`, `per-shot`, `report`, `chart`

## Context

Two display bugs were found in the v10 4K BBB per-shot report generated on
2026-05-18:

**Bug A — Bitrate column shows "—" for all shots.**
`vmaf-tune tune-per-shot` runs Phase-B bisect for each shot and emits a plan
JSON (`--plan-out`). The bisect result (`BisectResult`) carries `bitrate_kbps`
(measured from the encoded segment size), but `_build_per_shot_bisect_predicate`
returned only `(best_crf, measured_vmaf)` — matching the `PredicateFn` signature
`Callable[[Shot, float, str], tuple[int, float]]`. The `bitrate_kbps` value was
silently discarded, so the plan JSON had no `bitrate_kbps` field per shot. The
report ingester mapped the absent field to `float("nan")` and the renderer printed
"—" (em-dash) in the Bitrate column for every shot.

**Bug B — Per-shot timeline chart clips last shot.**
`_shot_plot_fn` in `report.py` set `ax.set_xlim(first_start - x_pad,
last_end + x_pad)` where `x_pad = max(1.0, 0.02 * span)`. When the last shot's
`end_frame` equals `last_end`, matplotlib's clip rectangle trims the rightmost
pixel of the `hlines` artist, making the final shot's CRF band invisible at small
output sizes or when the right edge coincides with the axis boundary.

Both bugs were user-discoverable at the per-shot report surface.

## Decision

**Bug A**: extend `ShotRecommendation` with an optional `bitrate_kbps: float`
field (default `float("nan")`). Introduce a `bitrate_sidecar` dict keyed by
`(start_frame, end_frame)` inside `_build_per_shot_bisect_predicate` and populate
it from `result.bitrate_kbps` as each shot's bisect completes. Change the return
type of `_build_per_shot_bisect_predicate` to
`tuple[PredicateFn, dict[tuple[int, int], float]]`. In `_run_tune_per_shot`,
annotate each `ShotRecommendation` with the captured bitrate (via
`dataclasses.replace`) after `tune_per_shot` returns. Emit the value as
`bitrate_kbps` (finite float or `null` for NaN) in the plan JSON. The
`PredicateFn` signature remains `Callable[[Shot, float, str], tuple[int, float]]`
to avoid breaking custom-predicate callers (`--predicate-module`).

**Bug B**: replace the symmetric `x_pad` with asymmetric padding:
`x_pad_left = max(1.0, 0.02 * span)` on the left side and
`x_pad_right = max(1.0, 0.05 * last_end)` on the right side. This guarantees
the last shot's `hlines` segment ends at least 5 % of the total frame count
inside the right viewport boundary, so it is always fully rendered.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Widen `PredicateFn` to return a 3-tuple `(crf, vmaf, bitrate)` | No side-channel needed | Breaks all existing custom predicates and the `_default_predicate` stub; requires a major API version bump | Breaking change with low ROI |
| Execute per-shot segment encodes after `tune_per_shot` to measure bitrate | Bit-exact bitrate from the actual final segment | Doubles total encode time; the bisect already encodes the best-CRF segment — re-encoding wastes resources | Unnecessary work |
| Symmetric x_pad on both sides for Bug B | Simpler code | Right side padding of 2 % is too small for short clips; the last shot still clips at some DPI settings | Insufficient fix |

## Consequences

- **Positive**: the Bitrate column in per-shot reports shows real kbps values;
  the per-shot chart shows all shots including the last one.
- **Negative**: `_build_per_shot_bisect_predicate` return type changes from
  `PerShotPredicateFn` to a 2-tuple — callers that assigned the return to a
  bare variable typed as `PerShotPredicateFn` will need updating (no such callers
  exist in tree outside of the tests now updated).
- **Neutral**: custom `--predicate-module` predicates do not carry bitrate data;
  the sidecar dict is empty for that path and `bitrate_kbps` is `null` / "—"
  in the report, which is the correct behaviour (no actual encode happened).

## References

- User request: per-shot bitrate column shows "—" for all shots in BBB v10 4K report.
- User request: per-shot chart cuts off the last shot's CRF transition.
- ADR-0513: prior per-shot 1-shot chart fix (`ax.hlines` bands, explicit xlim).
- ADR-0512: uniform-window splitter and scene threshold.
- `tools/vmaf-tune/src/vmaftune/per_shot.py` — `ShotRecommendation`.
- `tools/vmaf-tune/src/vmaftune/cli.py` — `_build_per_shot_bisect_predicate`, `_run_tune_per_shot`.
- `tools/vmaf-tune/src/vmaftune/report.py` — `_shot_plot_fn`.
- Tests: `tests/test_per_shot.py`, `tests/test_report.py`.
