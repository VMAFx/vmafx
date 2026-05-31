<!-- markdownlint-disable MD013 MD060 -->
# Research-0080: Encoder knob-sweep — populated Pareto hulls and recipe regressions

- **Status**: Findings ready
- **Date**: 2026-05-05
- **Companion ADRs**: [ADR-0305](../adr/0305-encoder-knob-space-pareto-analysis.md) (methodology), [ADR-0308](../adr/0308-encoder-knob-sweep-recipe-regression-policy.md) (regression-revision policy)
- **Companion digests**: [Research-0063](0063-encoder-knob-space-cq-vs-vbr-stratification.md) (CQ vs VBR stratification), [Research-0077](0077-encoder-knob-space-pareto-frontiers.md) (analysis scaffold)

## Summary

The 12,636-cell Phase A knob sweep
(`runs/phase_a/full_grid/comprehensive.jsonl`) was run end-to-end on a
single host across 9 sources × 6 hardware codecs × 3 rate-control
modes × ~78 knob combinations per slice. The Pareto-hull analysis
(`ai/scripts/analyze_knob_sweep.py`, ADR-0305) produced 162 realised
slices and flagged **1,915 recipe-vs-bare regressions** at the default
tolerances (`bitrate_tol_pct=5`, `vmaf_tol=0.1`). The headline finding
re-confirms Research-0063 with hard numbers: CQP regresses three times
less often than CBR or VBR, and h264_nvenc dominates the regression
distribution.

## Methodology recap

The full Pareto-hull derivation, slice stratification, and regression
detector are specified in [Research-0077](0077-encoder-knob-space-pareto-frontiers.md)
and [ADR-0305](../adr/0305-encoder-knob-space-pareto-analysis.md). This
digest does not duplicate either; it consumes the analyser unchanged
and reports what fell out. The only fork-local detail is field-name
adaptation: `hw_encoder_corpus.py` emits
`(src, actual_kbps, vmaf, enc_ms, recipe)` whereas `SweepRow` consumes
`(source, bitrate_kbps, vmaf_score, encode_time_ms, is_bare_default)`.
A throw-away wrapper (not committed) renamed the fields and synthesised
`is_bare_default = (recipe == 'bare')` before invoking the public
`analyze()` entry point. Producer-side schema alignment is tracked as
a follow-up — out of scope for this PR per
[ADR-0308](../adr/0308-encoder-knob-sweep-recipe-regression-policy.md).

## Headline findings

Per-codec aggregates over the realised slices (one row per codec; each
codec covers 27 slices = 9 sources × 3 rc_modes):

| codec        | slices w/ hull | max VMAF | bitrate p50 (kbps) | bitrate p95 (kbps) | encode time p50 (ms) | regressing recipes |
|--------------|---------------:|---------:|-------------------:|-------------------:|---------------------:|-------------------:|
| `av1_nvenc`  | 27 | 99.98 |  2,266 | 11,733 | 546 |   289 |
| `av1_qsv`    | 27 | 99.97 |  2,566 | 14,249 | 398 |    84 |
| `h264_nvenc` | 27 | 99.87 |  3,643 | 18,519 | 540 |   636 |
| `h264_qsv`   | 27 | 99.97 |  3,511 | 17,556 | 435 |   281 |
| `hevc_nvenc` | 27 | 99.93 |  3,537 | 16,553 | 543 |   515 |
| `hevc_qsv`   | 27 | 99.97 |  2,571 | 10,690 | 405 |   110 |

Aggregate totals: **12,636 rows**, **162 slices** (every slice has a
populated hull — there is no slice that collapsed to a single point),
**1,915 regressions**. Encode-time medians are reported in
milliseconds-per-clip and are dominated by Phase A's
`--sample-clip-seconds=10` window (ADR-0301): full-length encodes scale
roughly linearly.

### CQ vs VBR/CBR stratification (cites Research-0063)

| rc_mode | regressing rows | total rows | regression rate |
|---------|----------------:|-----------:|----------------:|
| `cqp`   |   278 | 4,212 |   6.6 % |
| `vbr`   |   787 | 4,212 |  18.7 % |
| `cbr`   |   850 | 4,212 |  20.2 % |

CQP regresses **~3× less often** than VBR or CBR at matched bitrate.
This re-confirms [Research-0063 §Decision](0063-encoder-knob-space-cq-vs-vbr-stratification.md):
the rate-control flip dominates the recipe-vs-default delta, and a
global hull (which Research-0063 rejected) would have collapsed the
two regimes and selected ship-candidate recipes that lose VMAF on the
CBR/VBR side. Under per-slice stratification the CQP recipes have
breathing room; under CBR/VBR most recipes that look attractive on the
CQP plot regress against the bare encoder default.

### Top 10 worst-offender recipes

Sorted by `vmaf_delta` ascending (deepest regression first); all rows
are h264_nvenc — the codec where the bare default is already
near-optimal at low bitrates and most non-trivial knob combinations
hurt:

| codec | rc | source | knob_combo | candidate VMAF | bare VMAF | Δ VMAF | matched bitrate (kbps) |
|-------|----|--------|-----------|---------------:|----------:|-------:|-----------------------:|
| `h264_nvenc` | vbr | FoxBird_25fps        | `preset=p4,q=1,recipe=spatial_aq` | 38.58 | 61.26 | -22.68 |   984 |
| `h264_nvenc` | cbr | Tennis_24fps         | `preset=p1,q=1,recipe=bf3`        | 56.90 | 75.75 | -18.85 |   912 |
| `h264_nvenc` | vbr | Tennis_24fps         | `preset=p1,q=1,recipe=bf3`        | 57.18 | 75.75 | -18.58 |   916 |
| `h264_nvenc` | vbr | BigBuckBunny_25fps   | `preset=p1,q=1,recipe=bf3`        | 44.05 | 61.40 | -17.34 |   940 |
| `h264_nvenc` | vbr | BigBuckBunny_25fps   | `preset=p1,q=1,recipe=full_hq`    | 45.39 | 62.56 | -17.17 |   984 |
| `h264_nvenc` | cbr | BigBuckBunny_25fps   | `preset=p1,q=1,recipe=bf3`        | 44.54 | 61.40 | -16.87 |   988 |
| `h264_nvenc` | vbr | Tennis_24fps         | `preset=p4,q=1,recipe=spatial_aq` | 59.52 | 75.75 | -16.23 |   924 |
| `h264_nvenc` | cbr | Tennis_24fps         | `preset=p4,q=1,recipe=spatial_aq` | 59.53 | 75.75 | -16.22 |   926 |
| `h264_nvenc` | cbr | BigBuckBunny_25fps   | `preset=p1,q=1,recipe=full_hq`    | 45.39 | 61.40 | -16.01 |   987 |
| `h264_nvenc` | vbr | FoxBird_25fps        | `preset=p1,q=2,recipe=spatial_aq` | 80.24 | 93.27 | -13.03 | 1,984 |

### Aggregated bad-recipe patterns

Aggregating the 1,915 individual regressions by `(codec, rc_mode,
recipe, preset, q)` (one row per cell, count = number of distinct
sources where the regression repeats, max 9), the top 15 cells are:

| codec | rc | recipe | preset | q | sources hit | mean Δ VMAF |
|-------|----|--------|--------|---|------------:|------------:|
| `h264_nvenc` | vbr | bf3        | p1 | 2 | 9 | -5.81 |
| `h264_nvenc` | vbr | bf3        | p1 | 4 | 9 | -3.05 |
| `h264_nvenc` | vbr | full_hq    | p7 | 1 | 9 | -4.12 |
| `h264_nvenc` | cbr | bf3        | p1 | 2 | 9 | -5.86 |
| `h264_nvenc` | cbr | spatial_aq | p1 | 2 | 9 | -3.30 |
| `h264_nvenc` | cbr | spatial_aq | p1 | 4 | 9 | -1.97 |
| `h264_nvenc` | cbr | spatial_aq | p1 | 8 | 9 | -0.95 |
| `h264_nvenc` | cbr | spatial_aq | p4 | 1 | 9 | -6.34 |
| `h264_nvenc` | cbr | spatial_aq | p4 | 4 | 9 | -2.45 |
| `h264_nvenc` | cbr | spatial_aq | p4 | 8 | 9 | -1.81 |
| `hevc_nvenc` | vbr | spatial_aq | p4 | 1 | 9 | -2.58 |
| `hevc_nvenc` | vbr | spatial_aq | p7 | 1 | 9 | -1.97 |
| `hevc_nvenc` | cbr | spatial_aq | p1 | 2 | 9 | -1.59 |
| `hevc_nvenc` | cbr | spatial_aq | p1 | 4 | 9 | -0.84 |
| `hevc_nvenc` | cbr | spatial_aq | p1 | 8 | 9 | -0.48 |

Every cell here regresses on **all 9 sources** in the corpus, so the
finding is corpus-content-independent and points to a codec/rc-mode
interaction with the knob set — not a content-dependent fluke.

## Recipe revisions (proposed for follow-up PRs)

The adapter package
([`tools/vmaf-tune/src/vmaftune/codec_adapters/`](../../tools/vmaf-tune/src/vmaftune/codec_adapters/))
does not currently bake regression-prone recipes as adapter-level
defaults — the recipes were enumerated by `hw_encoder_corpus.py` for
sweep coverage, not promoted into the adapter API. The findings below
are nonetheless load-bearing for any future *recommend*-style API or
documented "recommended" recipes:

1. **`h264_nvenc` + `bf3` at low CQ (`q ∈ {1, 2, 4}`) under VBR/CBR** —
   forbid as a default. Mean Δ VMAF ≈ -5.8 at `q=2`. The b-frame
   offset of 3 starves the rate controller at the constrained bitrate.
2. **`h264_nvenc` + `spatial_aq` under CBR (any preset, any q)** —
   forbid as a default. The pattern reproduces on **every** source in
   the corpus, mean Δ ranges from -0.95 to -6.34. NVENC's spatial AQ
   over-allocates bits to flat regions on h264 and reliably loses
   against the bare encoder.
3. **`h264_nvenc` + `full_hq` at `(preset=p7, q=1)` under VBR** —
   forbid. The "high-quality everything-on" preset stack regresses by
   -4.12 mean across all 9 sources. The marketing recipe loses to the
   bare default at matched bitrate.
4. **`hevc_nvenc` + `spatial_aq` under CBR/VBR at low q** — flag as
   discouraged-default. Smaller mean delta (-0.5 to -2.6) but
   reproduces on all 9 sources, so it is corpus-stable.
5. **All other recipes** — keep the bare encoder default
   (`recipe=bare`) as the documented baseline; opt-in recipes go
   through the per-slice hull lookup, never as a global default.

These revisions land in **follow-up PRs** that touch
`tools/vmaf-tune/codec_adapters/*` directly. This PR is documentation

- ADR only; per the task constraint, no adapter code is modified here.
[ADR-0308](../adr/0308-encoder-knob-sweep-recipe-regression-policy.md)
captures the decision-policy framing that gates those follow-ups.

## Limitations

- **Single-host variance**: the sweep ran on one machine; encode-time
  numbers are not reproducible across hosts (driver, thermals, PCIe
  contention, NVENC firmware revision). VMAF numbers are reproducible
  to within the per-frame noise floor. The regression detector uses
  VMAF only and is therefore robust to host variance.
- **9-source corpus skew**: all 9 sources are SDR Netflix Public
  Dataset clips at 24-30 fps. HDR sources, broadcast 50/60 fps, and
  UGC content are absent; recipes that lose on these 9 sources may
  win or lose differently on broader content. The corpus-content
  invariance of the top-15 aggregated patterns (every entry hits all
  9 sources) is reassuring but not a substitute for cross-corpus
  validation.
- **Sample-clip mode**: `--sample-clip-seconds=10` (ADR-0301) was
  used for the sweep to keep total wall time around 3 hours. Phase B
  / Phase C work that hits target-VMAF accuracy budgets will need a
  full-clip rerun for the recipes that survive this gate; the
  expected accuracy delta is 1-2 VMAF on diverse content per
  ADR-0301's published estimate.
- **Bare-default coverage**: the corpus emits 1,944 bare rows = 12 per
  slice on average. The regression detector reports a finding only
  when a bare row exists within `bitrate_tol_pct=5%` of the candidate;
  candidates outside that window are silently skipped (not falsely
  flagged). The coverage is dense enough that no slice missed all
  candidates, but isolated CRF rungs at the extremes of the bitrate
  range are not gated.
- **Knob-combo space is not exhaustive**: the 78-recipe enumeration
  per codec was hand-curated by `hw_encoder_corpus.py` and excludes
  `temporal_aq` × `spatial_aq` cross combinations beyond the
  `full` / `full_hq` aggregates, plus `bf` values outside `{0, 3, 4}`.
  Recipes that did not run cannot be flagged.
- **No per-frame VMAF**: the sweep emits scalar VMAF per encode. The
  regression detector cannot distinguish "uniformly worse" from
  "worse on a few frames"; both register as a Δ VMAF below threshold.
  Per-shot or per-frame analysis is a Phase C follow-up
  (Research-0063 §Future-work).

## References

- [Research-0063](0063-encoder-knob-space-cq-vs-vbr-stratification.md)
  — CQ vs VBR stratification finding (the original observation that
  motivated per-slice hulls).
- [Research-0077](0077-encoder-knob-space-pareto-frontiers.md) —
  analysis scaffold (Pareto-hull definition, regression detector,
  CSV / summary surfaces). PR #400.
- [ADR-0305](../adr/0305-encoder-knob-space-pareto-analysis.md) —
  per-slice stratification decision. PR #400.
- [ADR-0308](../adr/0308-encoder-knob-sweep-recipe-regression-policy.md)
  — regression-revision policy. This PR.
- `runs/phase_a/full_grid/comprehensive.jsonl` (gitignored) —
  12,636-row sweep file. Not committed; reproduce locally via
  `tools/vmaf-tune/src/vmaftune/hw_encoder_corpus.py` per ADR-0297 +
  ADR-0301.
- `runs/phase_a/full_grid/reports/summary.md` (gitignored) —
  generated per-slice hull summary; this digest is the committed
  reading surface.
