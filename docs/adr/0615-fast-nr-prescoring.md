<!-- markdownlint-disable MD060 -->
# ADR-0615: Fast NR Pre-Scoring for CRF Bisect Acceleration

- **Status**: Proposed
- **Date**: 2026-05-19
- **Deciders**: lusoris
- **Tags**: `ai`, `planning`, `vmaf-tune`

## Context

Full-reference VMAF (FR VMAF) requires decoding both reference and distorted
YUV, then running 6 feature extractors frame-by-frame. On a 10-second HD shot
a single FR VMAF call takes 2–5 seconds on CPU. The Phase B bisect makes 5–8
calls per shot; a 500-shot title costs 10,000–20,000 VMAF-seconds of scoring.
The in-tree `nr_metric_v1.onnx` model scores the distorted stream alone, with
no reference decode, and runs in <200 ms per shot. Using it as a coarse proxy
for early bisect iterations can cut wall-time substantially.

## Decision

We will implement an NR-early-elimination strategy (Research 0611 Option B):
at each bisect midpoint, compute the NR score. If `|NR - target| > δ_fast`
(calibrated threshold, default 8 VMAF), skip the FR call and proceed in the
NR-implied direction. If within the uncertainty zone, pay the FR cost. The
threshold `δ_fast` is calibrated on the Netflix corpus via a fit of
`vmaf_fr ≈ f(vmaf_nr)`, with `δ_fast = 2σ` of the residual. An
`NRProxyBackend` class is added to `score_backend.py` to encapsulate this
logic; the seam is the existing backend selector.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|--------|------|------|----------------|
| A — NR-only bisect + FR confirm on final CRF | Maximum speedup (3–6×) | Risk of selecting wrong CRF on NR-biased content | Correctness risk too high without calibration validation |
| B — NR early elimination (chosen) | Correct by construction at boundary; 2–4× speedup | Two-model invocation; requires calibrating δ_fast | — |
| C — conformal-calibrated NR threshold | Principled UQ; lowest false-pass rate | 1–2 weeks extra for conformal calibration | Deferred as V2 upgrade path |
| D — bitstream-feature NR (no decode) | 5–10× speedup | Encoder-specific parsers; non-portable | Significant engineering; deferred |

## Consequences

- **Positive**: 2–4× bisect wall-time reduction on in-domain content; directly
  enables the DO (ADR-0613) and per-shot ABR (ADR-0614) items whose O(candidates)
  inner loops become tractable.
- **Negative**: Two-model inference adds ~200 ms overhead on shots where FR would
  have been sufficient; calibration requires a one-time corpus sweep.
- **Neutral / follow-ups**: `δ_fast` must be documented in `model/tiny/nr_metric_v1.json`
  as a new `calibration_threshold` field. Conformal upgrade (Option C) is a natural
  V2.

## Dependencies

- `model/tiny/nr_metric_v1.onnx` — in-tree; no new model required.
- `tools/vmaf-tune/src/vmaftune/score_backend.py` — integration point.
- Netflix corpus (`.workingdir2/netflix/`) — for calibration sweep.

## Implementation phases

| Phase | Description | Effort |
|-------|-------------|--------|
| P1 | `NRProxyBackend` in `score_backend.py`; unit tests with stub NR model | 1 day |
| P2 | Calibration sweep script against Netflix corpus; compute and record `δ_fast` | 1 day |
| P3 | Wire `NRProxyBackend` into Phase B bisect as opt-in `--fast-nr` flag | 0.5 day |
| P4 | Docs `docs/usage/vmaf-tune-fast-nr.md` | 0.5 day |

Total estimate: 3 days.

## References

- Research digest: [docs/research/0611-fast-nr-prescoring-research.md](../research/0611-fast-nr-prescoring-research.md).
- `model/tiny/nr_metric_v1.onnx`, `model/tiny/nr_metric_v1.json`.
- `tools/vmaf-tune/src/vmaftune/score_backend.py`, `bisect.py`.
- `tools/vmaf-tune/src/vmaftune/conformal.py` — future conformal upgrade path.
- Source: per user direction (roadmap planning session 2026-05-19).
