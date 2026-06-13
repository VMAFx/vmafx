<!-- markdownlint-disable MD013 MD060 -->
# ADR-0534: vmaf-tune compare emits + renders rate-quality curve from per-iteration bisect samples

- **Status**: Accepted (target-VMAF defaults superseded by [ADR-0538](0538-premium-vmaf-target-defaults-and-bisect.md))
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude
- **Tags**: vmaf-tune, compare, report, chart, ux

## Context

Two related issues surfaced in the BBB e2e v10 4K codec-comparison report (PR #1276, ADR-0516):

1. **Default `--target-vmafs` are unrealistic for streaming.** The default sweep `85,90,92,95` covers premium-tier quality only. Realistic streaming operating points span VMAF 70-90 (broadcast and low-bandwidth live streaming live there). The single-point 92 is especially misleading, and VMAF 95+ frequently exceeds the codec's CRF ceiling — the bisect returns "unreachable" rows that pollute the report instead of producing data.
2. **The rate-quality chart connects mismatched-target points and produces physically impossible downward dips.** `_sweep_plot_fn` in `tools/vmaf-tune/src/vmaftune/report.py` plotted one point per (codec, target_vmaf) and connected per-codec points by ascending `target_vmaf`. Y-axis is "VMAF achieved" — but the bisect overshoots/undershoots different amounts at each target, so the achieved VMAF can decrease as the requested target increases (e.g. libx265 BBB v10: target 92 → achieved 90.5, target 95 → achieved 95.3). The resulting downward slopes read as "more bitrate → less quality", which is physically wrong.

The bisect already probes 3-5 CRFs per (codec, target) — every probe is a genuine R-Q measurement on the codec under test, with no overshoot bias. Plumbing those probes through to the report turns the chart into a real per-codec rate-quality curve.

## Decision

We will:

1. **Plumb bisect samples through `compare`.** `bisect_target_vmaf` records every successful encode+score round-trip into `BisectResult.samples`, exposed as a new `BisectSample` dataclass. The compare adapter (`BisectResult.to_recommend_result`) projects these into `RecommendResult.bisect_samples` (a tuple of dicts), which `to_row` emits into the v2 JSON only when populated (additive, schema-compatible).
2. **Render the rate-quality chart from those samples.** `_sweep_plot_fn` aggregates `bisect_samples` per codec across all targets, de-duplicates by CRF, sorts by bitrate, and draws a single monotonic-friendly R-Q curve per codec. Picked-CRF rows are overlaid as larger circled markers. The pareto frontier still comes from the picked-CRF rows.
3. **Back-compat path with caveat.** v1 schemas and v2 schemas without `bisect_samples` (older v10 reports) still render via the legacy connect-the-dots line with a caveat note in the title (`caveat: connect-the-dots may show overshoot artefacts`).
4. **Realistic default for `--target-vmafs`.** Change `compare`'s default from `85,90,92,95` to `75,80,85,90,93` — covers low-end streaming through premium. The top stops at 93 because 95+ frequently misses the codec's CRF ceiling. The new default is non-empty, so the existing legacy single-target path is preserved via a `_TrackedDefaultAction` sentinel: when `--target-vmaf NN` is passed explicitly and `--target-vmafs` is left at its default, the v1 schema is emitted (back-compat for scripts pinning a single VMAF).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Scatter-only (drop the lines, plot each picked-CRF as a marker) | Trivial change; no plumbing; no misleading line | Loses the rate-quality story; user can't see codec curves; pareto-frontier is the only line | Less informative; doesn't surface the codec's intrinsic R-Q shape |
| **From-bisect-samples (chosen)** | Plots genuine codec R-Q curves; no overshoot artefact; picked-CRF still highlighted; pareto preserved | Schema additive change; touches bisect + compare + report + CLI ingest | Strictly more informative; the bisect already computed these — just plumbing |
| Hull-fit per codec (fit a smooth log-linear hull through the picked points) | Smooth curve; one line per codec | Hides the discrete CRF cells the bisect actually probed; the "fit" is fiction unless calibrated | Adds a model with no evidence it matches the codec; defeats the "real measurements" purpose |
| Keep defaults at `85,90,92,95` | No CLI surface change | Premium-only sweep; ignores the broadcast / low-streaming operating range | The whole point of the compare sweep is to inform encoding decisions; defaults that don't match real operating points are misleading |

## Consequences

- **Positive**: rate-quality chart now shows genuine codec curves; no impossible downward dips. Default sweep covers realistic streaming targets. Bisect samples are exposed for downstream analysis (e.g. operator scripts that want the raw R-Q points).
- **Negative**: v2 JSON payloads grow by ~3-5 sample dicts per (codec, target) row (~few KB per sweep). CSV intentionally drops the structured column (`extrasaction="ignore"` on the DictWriter) so the flat row contract is preserved.
- **Neutral / follow-ups**: existing v10 reports (no `bisect_samples`) still render via the legacy chart with the caveat note — operators can opt in by regenerating the sweep with the new CLI. The new `BisectSample` and `BisectSamplePoint` types are exported from `vmaftune.bisect` and `vmaftune.report` respectively.

## References

- PR #1276 (ADR-0516) — multi-target sweep that introduced the bug pattern.
- BBB 4K v10 report — the libx265 target 92 → 90.5 / target 95 → 95.3 case that motivated this ADR.
- `req` (direct user request, 2026-05-18): "Pick option 2 — it's strictly more informative AND avoids the misleading connect-the-dots artifact. The bisect already probes 3-5 CRFs per (codec, target); just emit them in the JSON and the report renders them."
- `req`: "Change default to a wider realistic range: 75,80,85,90,93 (5 points covering low-end streaming through premium). The top end stops at 93 (not 95) because 95+ requires bisect to push CRF too low and often misses, producing useless rows."
