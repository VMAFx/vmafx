<!-- markdownlint-disable MD013 -->
# ADR-0293: `vmaf-tune` saliency-aware ROI tuning (Bucket #2)

- **Status**: Accepted
- **Date**: 2026-05-03
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: tooling, ai, saliency, ffmpeg, codec, fork-local

## Context

The fork ships two saliency surfaces today: `mobilesal` /
`saliency_student_v1` as a libvmaf feature extractor (scoring side,
ADR-0218 / ADR-0286), and `vmaf-roi` as a C sidecar binary that
emits per-CTU QP-offset files for x265 / SVT-AV1 (encoder side,
ADR-0247). Bucket #2 of the PR #354 audit calls for a *third*
surface: the same saliency signal exposed through the
[`vmaf-tune`](../usage/vmaf-tune.md) Python harness so a single
command can produce a saliency-biased encode end-to-end.

The umbrella decision (ADR-0237) carved `vmaf-tune` into six phases.
Phase A (the grid-corpus scaffold, PR #329) shipped the codec
adapter contract, the JSONL row schema, and the subprocess seam.
Bucket #2 is a tactical add-on inside Phase A's footprint — a
`recommend` subcommand and a `saliency.py` module — that does not
require Phase B (target-VMAF bisect). The `recommend` flag surface
is wired so Phase B can swap in a true bisect later without renaming
flags.

The fork-trained `saliency_student_v1` weights (ADR-0286, PR #359)
ship under BSD-3-Clause-Plus-Patent and unblock this work — earlier
attempts to source MobileSal upstream weights were blocked by
license incompatibility (ADR-0257).

## Decision

We will add `tools/vmaf-tune/src/vmaftune/saliency.py` and a new
`vmaf-tune recommend` CLI subcommand that:

1. Computes a per-clip aggregate saliency mask from
   `saliency_student_v1.onnx` over a sampled set of frames.
2. Maps the mask to a per-pixel QP-offset map clamped to ±12 (matching
   `vmaf-roi`'s ADR-0247 convention).
3. Reduces to per-MB granularity (16×16 luma) for x264 `--qpfile`.
4. Runs a single ffmpeg encode with the qpfile injected via
   `-x264-params qpfile=…`.

The model is loaded lazily and is **optional**: missing
onnxruntime / missing weights logs a warning and falls back to a
plain encode so the harness always returns a result. All numeric
kernels (RGB conversion, ImageNet normalisation, per-MB reduce,
QP clamp) are pure NumPy so the test suite runs without
onnxruntime; the ONNX session is mocked via a `session_factory`
seam, mirroring the existing subprocess seam in `encode.py` /
`score.py`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Shell out to the existing `vmaf-roi` C binary | Reuses ADR-0247 wiring; one source of truth for the saliency→QP math; immediately covers x265 + SVT-AV1 | Requires a built libvmaf tree on PATH; binary is one-frame-per-invocation today (Wave 1 deliberately) so per-clip aggregate would need a per-frame loop in Python *plus* a separate aggregation step; harder to mock in tests | Wave-2 follow-up: once `vmaf-roi` grows a batch mode (roadmap §2.3), the Python helper can delegate. For Bucket #2 a self-contained Python path is cheaper to ship and test. |
| Pure-Python ONNX inference (chosen) | Zero binary dependency on `vmaf-roi`; clean test-seam (mocked session); same numeric pipeline as the C side | Duplicates the saliency→QP math (small: ~5 lines numpy); needs `onnxruntime` at runtime; today only emits x264 qpfile | Selected — graceful fallback covers the missing-onnxruntime case; codec coverage extends one file per encoder under `codec_adapters/` without touching the search loop. |
| Bake saliency into the existing `corpus` subcommand | One subcommand, no API growth | Conflates "sweep a grid" with "produce a recommended encode"; corpus rows would gain a saliency-on/off bool that downstream Phase B/C would have to special-case | The `recommend` subcommand is the right layer — Phase B's target-VMAF bisect will land here too. |

## Consequences

- **Positive**: end-to-end saliency-aware encoding becomes a single
  command (`vmaf-tune recommend --saliency-aware`); no manual
  per-frame qpfile orchestration required. The `recommend` flag
  surface is now stable for Phase B's bisect drop-in.
- **Positive**: Bucket #2 of the PR #354 audit is closed without
  blocking on `vmaf-roi` Wave-2 batch mode.
- **Negative**: a small numeric duplication of the saliency→QP map
  with `vmaf-roi`'s C implementation. Both clamp to ±12 with a
  signed `centred = 2*sal − 1` linear blend, so the bit-for-bit
  contract is one assertion in `test_saliency.py`.
- **Negative**: x264-only in this PR. x265 / SVT-AV1 inherit
  `vmaf-roi`'s sidecar today and will get the `vmaf-tune recommend`
  variant in a one-file follow-up (`codec_adapters/x265.py` +
  qpfile formatter).
- **Neutral / follow-ups**: Phase B (target-VMAF bisect) replaces
  the explicit-CRF default with a real bisect; Phase C (per-shot
  CRF predictor) consumes per-frame saliency rather than
  per-clip aggregate; integration coverage with a real ffmpeg +
  real model lands when the codec adapter set widens.

## References

- [ADR-0237](0237-quality-aware-encode-automation.md) — `vmaf-tune` umbrella decision.
- [ADR-0286](0286-saliency-student-fork-trained-on-duts.md) — `saliency_student_v1` (fork-trained, PR #359 — assigns the ADR ID via that PR's index fragment).
- [ADR-0247](0247-vmaf-roi-tool.md) — `vmaf-roi` C sidecar (signal blend, clamp window, sidecar formats).
- [ADR-0218](0218-mobilesal-saliency-extractor.md) — scoring-side saliency extractor.
- [ADR-0257](0257-mobilesal-real-weights-deferred.md) — why upstream MobileSal weights were rejected (license).
- [Research-0046](../research/0046-vmaf-tune-saliency-roi.md) — bucket #2 design digest (this PR).
- Source: PR #354 audit Bucket #2 (paraphrased: wire saliency-aware ROI into `vmaf-tune`'s recommend path).
