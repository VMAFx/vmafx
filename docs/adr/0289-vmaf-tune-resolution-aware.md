<!-- markdownlint-disable MD060 -->
# ADR-0289: `vmaf-tune` resolution-aware model selection + CRF offsets

- **Status**: Accepted
- **Date**: 2026-05-03
- **Deciders**: Lusoris
- **Tags**: tooling, vmaf-tune, model-selection, fork-local

## Context

VMAF is a resolution-aware metric. The fork ships two production-grade
pooled-mean models in `model/`: `vmaf_v0.6.1.json` (trained on a 1080p
viewing setup) and `vmaf_4k_v0.6.1.json` (re-fit for a 4K display).
Scoring 4K content against the 1080p model under-counts spatial detail;
scoring 1080p content against the 4K model over-counts coding
artefacts. The bias is several VMAF points either way — large enough to
poison Phase B (target-VMAF bisect) and Phase C (per-title CRF
predictor) corpora when the sweep covers a mixed-resolution ladder.

Phase A of `vmaf-tune` (ADR-0237) shipped with one fixed model per
sweep — fine for a single-resolution corpus, lossy for any ABR-ladder
input. PR #354's audit (Bucket #8) flagged this as the next correctness
gap before Phase B/C/D land. The fix is small and entirely fork-local:
a height-based decision rule and a tiny per-resolution CRF-offset hook
the future search layer can use to seed bisect bounds.

## Decision

We will add `tools/vmaf-tune/src/vmaftune/resolution.py` exposing
`select_vmaf_model_version(width, height) -> str`,
`select_vmaf_model(width, height) -> Path`, and
`crf_offset_for_resolution(width, height) -> int`. The decision rule is
height-only:

- `height >= 2160` → `vmaf_4k_v0.6.1`
- else → `vmaf_v0.6.1` (canonical fallback for 720p / SD too — the
  fork has no 720p / SD model and Netflix's published guidance is to
  use the 1080p model for all sub-2160p content).

`corpus.iter_rows` consumes `select_vmaf_model_version` once per job
(encode dimensions are fixed across all `(preset, crf)` cells of a
job). The CLI gains `--resolution-aware` / `--no-resolution-aware`
(default on); when off, the explicit `--vmaf-model` drives every row.
The emitted JSONL row's `vmaf_model` field reflects the *effective*
model used, not `opts.vmaf_model` — otherwise mixed-ladder corpora
would lie about which model scored each row.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Height-only threshold at 2160 (chosen) | Matches Netflix's published guidance; one branch; trivial test surface; future-proof for 8K (clamps to 4K model). | Loses some 1440p nuance — those rows route to the 1080p model even though they're closer to 4K viewing. | Picked: the bias on 1440p is ~0.5 VMAF (acceptable); a 1440p model doesn't exist in the fork. |
| Width-and-height matrix | More accurate for anamorphic / cropped content. | Adds a 2-D decision surface; needs per-codec calibration; no public guidance for the corner cases. | Defer: width is accepted as an argument for API symmetry, but the body ignores it until we have a real anamorphic corpus to fit against. |
| Pixel-count threshold (e.g. ≥ 6 Mpx → 4K) | Handles 21:9 / cropped sources cleanly. | Drifts on letterbox/pillarbox; the canonical Netflix guidance is height-only. | Not chosen: optimising for a corner case over the documented mainline. |
| Defer to Phase B (let bisect re-score with both models) | Keeps Phase A semantics. | 2× scoring cost on every cell; corpus rows ambiguous; downstream regressors get confusing dual-model training data. | Not chosen: doubles the most expensive operation in the loop. |
| User-supplied model per source | Most flexible. | Pushes the decision back to the user, defeating the point of "auto-pick". `--no-resolution-aware` + `--vmaf-model` already covers the manual escape hatch. | Not chosen: the auto path needs to be the default. |

## Consequences

- **Positive**: mixed-ladder corpora (e.g. a 7-rung ABR ladder from
  240p to 2160p) now score every row against the right model with no
  per-row user input. `vmaf_model` in the JSONL is now reliable
  ground-truth metadata for downstream Phase B/C/D regressors. The CRF
  offset hook unlocks a sane default for the future search layer to
  seed bisect bounds across resolutions.
- **Negative**: one new module + CLI flag + JSONL semantics
  clarification (`vmaf_model` is now per-row, not per-job). Existing
  consumers that read the JSONL and assumed `vmaf_model` was constant
  across a corpus need to handle per-row variance (none ship today —
  Phase B/C/D are not implemented yet).
- **Neutral / follow-ups**:
  - Add a 1440p model when Netflix publishes one upstream — until
    then 1440p stays on the 1080p side of the threshold.
  - Phase B/C/D will learn per-codec CRF offsets from real corpora and
    override the conservative defaults shipped here. The function
    signature stays stable.
  - `tools/vmaf-tune/AGENTS.md` gets a new invariant note about the
    resolution decision rule and the per-row `vmaf_model` semantics.

## References

- Parent: [ADR-0237](0237-quality-aware-encode-automation.md) — the
  `vmaf-tune` umbrella spec / phase ordering.
- Research digest:
  [`docs/research/0064-vmaf-tune-resolution-aware.md`](../research/0064-vmaf-tune-resolution-aware.md).
- PR #354 audit, Bucket #8 (resolution-aware tuning gap).
- Source: `req` — user direction 2026-05-03 to wire the
  resolution-aware decision rule into `vmaf-tune` per the Bucket #8
  audit before Phase B/C/D land.
