<!-- markdownlint-disable MD060 -->
# ADR-0222: `vmaf-perShot` per-shot CRF predictor sidecar

- **Status**: Accepted
- **Date**: 2026-04-29
- **Deciders**: Lusoris
- **Tags**: ai, tools, encoder-hint, fork-local, t6-3b

## Context

The fork's tiny-AI roadmap §2.4 (per-shot CRF predictor + TransNet V2
shot boundaries) decomposes into two tasks tracked in
[`.workingdir2/BACKLOG.md`](../../.workingdir2/BACKLOG.md):

- **T6-3a** — TransNet V2 shot-boundary feature extractor (~1M params,
  100-frame window). Already proposed as PR #210 / ADR-0220 — emits
  per-frame `shot_boundary_probability` and a binary `shot_boundary`
  flag.
- **T6-3b** — *this ADR* — the standalone CLI `tools/vmaf-perShot`
  that consumes a YUV reference, segments it into shots, computes
  per-shot signal aggregates (scene complexity, motion energy, length),
  and emits a per-shot CRF plan (CSV / JSON) for downstream encoders.

The split keeps each side independently mergeable. T6-3b ships a
**fallback frame-difference shot detector** so the tool is useful
without T6-3a having merged yet; once T6-3a lands the tool can take
a pre-computed shot map and skip its built-in detector. The CRF
predictor itself stays inside the sidecar — it never enters
libvmaf's metric path.

## Decision

We will ship `vmaf-perShot` as a standalone executable under
`core/tools/vmaf_per_shot.c` with the following v1 contract:

- **Input**: planar YUV420P (8 / 10 / 12-bit), a target VMAF, and CRF
  clamp bounds.
- **Shot detector**: per-frame mean absolute luma delta vs. the
  previous frame, normalised to the 8-bit domain. Cuts trigger when
  the delta exceeds `--diff-threshold` (default `12.0`) and the
  running shot has reached `VMAF_PER_SHOT_MIN_LEN = 4` frames
  (suppresses flash / fade flicker).
- **Per-shot signals**: `mean_complexity` (sample variance of luma)
  and `mean_motion` (mean absolute frame delta) averaged over the
  shot's frames.
- **CRF predictor** (v1, transparent linear blend):

  ```text
  crf = base
        + 0.20 * range * motion_norm * length_factor
        - 0.20 * range * complexity_norm
        - 0.15 * range * target_norm
  ```

  where `range = crf_max - crf_min`, `base = crf_min + range/2`,
  signals are clipped into `[0, 1]`, `length_factor = 0.5` for shots
  < 24 frames (else 1.0). Output clamps into `[crf_min, crf_max]`.
- **Output**: CSV (default) or JSON, one row / object per shot with
  the full signal vector so downstream encoders can override the
  predicted CRF with their own rule.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Linear-blend heuristic (chosen) | Transparent; debuggable; no training corpus needed; runs in 30 ms on a 48-frame fixture | Coefficients are a prior, not a fit | Roadmap §2.4 says "v1 = small CNN/MLP"; without a labelled corpus a "small MLP" would just memorise the prior. Defer the MLP to v2 once the BVI-DVC + Netflix corpora produce shot-level CRF labels |
| Trained MLP (PyTorch → ONNX) | Closer to roadmap §2.4's "small CNN/MLP" wording; later versions could trade accuracy for size | No labelled per-shot CRF dataset exists today; the existing tiny-AI corpora (BVI-DVC, Netflix) have per-clip / per-frame labels, not per-shot CRF targets | Forces fabricating labels; adds ORT dependency to the sidecar for a v1 that has no measured advantage over the linear blend |
| Embed as a libvmaf C-API entry point (`vmaf_per_shot_predict`) | Reusable from FFmpeg / Python | Stretches libvmaf's "metric engine" charter into encoder-hint territory; CLI flag set is large enough that an embedded API would still need a CLI wrapper | Roadmap §2.4 explicitly says "**Standalone CLI** (`tools/vmaf-perShot`) that writes an encoder-ingestible sidecar. Does **not** run inside libvmaf — its output is a parameter hint, not a quality score" |
| CSV-only output | Simplest tooling | Encoders that consume JSON sidecars (some webapp pipelines) need a second tool | Both formats are 5 lines of code each; ship both |
| JSON-only output | Modern; nestable | Bash / awk pipelines have to install `jq` | CSV is the path-of-least-resistance for shell-driven encode farms |
| SI / TI complexity (Spatial / Temporal Information per ITU-T P.910) | Industry-standard for content classification | Two extra Sobel-filter passes per frame; v1 doesn't need that fidelity | Frame-variance + mean-abs-delta are correlated enough with SI / TI for a v1 prior; revisit if the linear blend's empirical PLCC vs. real CRF labels comes back below 0.7 |

## Consequences

- **Positive**.
  - Encoder pipelines can run `vmaf-perShot` against a reference YUV
    and feed the resulting CRF plan into x264 / x265 / SVT-AV1
    `--zones` (or equivalent) without involving the libvmaf metric
    path.
  - The sidecar contract (CSV / JSON columns: `shot_id`,
    `start_frame`, `end_frame`, `frames`, `mean_complexity`,
    `mean_motion`, `predicted_crf`) is stable across v1; v2's
    trained MLP can replace `predicted_crf` without touching
    columns or the CLI flag set.
  - T6-3b unblocks the CRF-prediction half of roadmap §2.4
    independently of T6-3a's merge state.
- **Negative**.
  - The frame-difference detector is less accurate than TransNet V2
    on dissolves / fades / cross-fades; the v1 plan will sometimes
    under-segment those transitions. Documented in
    [`docs/usage/vmaf-perShot.md`](../usage/vmaf-perShot.md).
  - The linear-blend predictor encodes a static prior; a real
    encode will only validate it against a target VMAF, not
    optimise over the trade-off.
- **Neutral / follow-ups**.
  - **v2 trained predictor** (separate ADR): wire a per-shot MLP
    once a labelled corpus exists; reuse the same CSV / JSON
    schema so consumers don't churn.
  - **TransNet V2 wiring** (T6-3a follow-up): once the extractor
    merges, the tool will gain `--shots PATH` to consume a
    pre-computed shot map and bypass the heuristic detector.
  - **Higher-fidelity complexity** (SI / TI): if v2's empirical
    PLCC against real CRF labels falls below the target, swap the
    variance signal for SI and the abs-diff signal for TI.

## References

- Roadmap: [`docs/ai/roadmap.md`](../ai/roadmap.md) §2.4.
- Sister ADR: [ADR-0220](0220-transnet-v2-shot-detector.md) (T6-3a,
  TransNet V2 extractor, in-flight).
- Backlog: [`.workingdir2/BACKLOG.md`](../../.workingdir2/BACKLOG.md)
  row T6-2 part b T6-3b.
- Source: `req` — direct user direction, scoping note in T6-3b
  briefing 2026-04-29.
