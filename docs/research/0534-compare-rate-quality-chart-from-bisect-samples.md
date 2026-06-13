<!-- markdownlint-disable MD013 MD060 -->
# Research-0530: compare rate-quality chart from bisect samples

- **Status**: Active
- **Workstream**: ADR-0530
- **Last updated**: 2026-05-18

## Question

The `vmaf-tune compare` v2 rate-quality chart connects per-codec picked-CRF points by ascending `target_vmaf`, which produces physically impossible downward dips when the bisect overshoots one target by more than it overshoots an adjacent one. What is the right representation: scatter-only, from-bisect-samples, or a hull-fit? And what is the right default `--target-vmafs` for a streaming-grade comparison?

## Sources

- PR #1276 (`fix(vmaf-tune,libvmaf): close BBB e2e v10 bug cluster (ADR-0516)`) — introduced the multi-target sweep and the bar-style chart.
- BBB 4K v10 report (`.workingdir/bbb_reports/bbb_2160p60_v10_*.{html,md}`) — concrete data: libx265 target 92 → achieved 90.5, target 95 → achieved 95.3, producing a visible downward slope.
- `tools/vmaf-tune/src/vmaftune/bisect.py` — the bisect already records 3-5 encode+score round-trips per (codec, target) inside `bisect_target_vmaf`. Each is a genuine bitrate/VMAF measurement with no overshoot bias.
- Netflix VMAF README — operator-facing guidance on streaming-quality VMAF tiers (broadcast ≈ VMAF 70-85; streaming ≈ 80-90; archival/source ≈ 95+).
- BVI-DVC / Netflix Public Dataset benchmarks (informally referenced in `tools/vmaf-tune/AGENTS.md` — codec adapter discipline).

## Findings

- **The downward-dip artefact is a presentation bug, not a measurement bug.** Every probe the bisect records is a real (CRF, bitrate, VMAF) point on the codec's R-Q surface. The bisect picks the highest-CRF cell whose measured VMAF clears the target — which by construction can overshoot (the next CRF below would have undershot). Plotting only the picked cell + connecting per-codec by target VMAF cannot recover the true codec curve.
- **From-bisect-samples is strictly more informative than scatter-only.** A 4-codec × 5-target sweep produces 4×5 = 20 picked points and roughly 4×5×3.5 ≈ 70 bisect probes. The 70 probes already form a per-codec curve at no additional encode/score cost.
- **Hull-fit (smooth log-linear regression through picked points) is fiction unless calibrated.** The fitted line says nothing about the codec's intrinsic R-Q shape — it would smooth over real plateaus or kinks. Rejected.
- **Realistic streaming sits at VMAF 75-90.** The default `85,90,92,95` is a premium-archival sweep that wastes the bottom half of the chart. The new default `75,80,85,90,93` covers the streaming dial; 95+ frequently exceeds the codec's CRF ceiling (libx264/libx265 cap at CRF 0 long before VMAF 95 in 4K high-motion content) and produces "unreachable" failure rows.

## Alternatives explored

| Option | Verdict | Reason |
|---|---|---|
| Scatter-only (drop the line) | Rejected | Loses the codec R-Q narrative |
| **From-bisect-samples** | **Chosen** | Genuine codec curves at zero extra encode cost |
| Hull-fit (regression) | Rejected | Fits a story to the data instead of plotting it |
| Keep defaults at `85,90,92,95` | Rejected | Premium-only sweep mis-represents streaming reality |
| Defaults at `70,80,85,90,93` (start at 70) | Considered | 70 is rarely useful as an absolute floor — VMAF 75 is the broadcast threshold |
| Defaults at `75,80,85,90,93,95` (include 95) | Rejected | 95+ "unreachable" rows pollute the report on most non-archival sources |

## Implementation summary

- `BisectSample` dataclass added to `vmaftune.bisect`; `bisect_target_vmaf` appends every successful probe; `BisectResult.samples` carries the tuple.
- `BisectResult.to_recommend_result()` projects samples into `RecommendResult.bisect_samples` (tuple of dicts).
- `RecommendResult.to_row()` emits `bisect_samples` only when populated (additive — v1 + v2-without-samples consumers see no schema delta).
- `CodecSweepPoint` gains an optional `bisect_samples: tuple[BisectSamplePoint, ...]` field. The CLI's `_sweep_point_from_json` parses the field when present.
- `_sweep_plot_fn` aggregates samples per codec across all targets, deduplicates by CRF, sorts by bitrate, plots the curve with small markers, and overlays picked-CRF rows as larger circled markers. Pareto frontier unchanged (always from picked-CRF rows). Legacy path retained with caveat note in the title.
- CLI default `--target-vmafs` flipped to `75,80,85,90,93` via `_TrackedDefaultAction` so legacy single-target callers (`--target-vmaf 92` alone) keep their v1 emission.

## Verification

- `tests/test_compare_rate_quality_sweep.py` adds: (1) bisect records samples on successful target hits; (2) `to_row` emits `bisect_samples` only when populated; (3) v2 JSON round-trips samples; (4) CSV drops the structured column; (5) sweep-point ingester parses + back-compat; (6) HTML report carries the new chart phrasing when samples present; (7) HTML carries the caveat phrasing when samples absent; (8) CLI default sweep matches `75,80,85,90,93`; (9) CLI back-compat for `--target-vmaf` alone.
- BBB 4K v11 sweep regenerated under `.workingdir/bbb_reports/bbb_2160p60_v11_*.{html,md}` to visually confirm monotonic curves + no impossible dips.
