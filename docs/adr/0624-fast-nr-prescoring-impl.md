<!-- markdownlint-disable MD060 -->
# ADR-0624: Fast NR Pre-Scoring Implementation (ADR-0615 impl)

- **Status**: Accepted (Implemented)
- **Date**: 2026-05-19
- **Deciders**: lusoris
- **Tags**: `ai`, `vmaf-tune`, `bisect`, `onnx`, `fork-local`

## Context

ADR-0615 decided to use an NR early-elimination strategy for the Phase B CRF
bisect: at each midpoint, score the distorted stream with `nr_metric_v1.onnx`
(~200 ms CPU, <50 ms GPU EP), and skip the full-reference VMAF call when
`|NR - target| > δ_fast`. The implementation phases were:

- **P1** — `NRProxyBackend` class in `score_backend.py` with per-CRF cache and
  ONNX session lifecycle (CUDA/ROCm EP with CPU fallback).
- **P2** — `ai/scripts/calibrate_nr_threshold.py` calibration sweep.
- **P3** — `--fast-nr` CLI flag on `compare` and `tune-per-shot`; threading
  through `make_bisect_predicate` and `_build_per_shot_bisect_predicate`.
- **P4** — Docs `docs/usage/vmaf-tune-fast-nr.md`.

The `score_backend.py` module was already the natural seam (ADR-0315); the
`NRProxyBackend` class sits alongside the existing FR backend selector without
touching the shared backend-selection logic.

## Decision

Implement ADR-0615 Option B (NR early-elimination) across four phases as
scoped above. Key design choices made during implementation:

1. **Inference error isolation**: `session.run()` exceptions are wrapped in
   `NRProxyBackendError` so the bisect loop can safely fall through to FR on
   any inference failure — zero risk of silent correctness errors.
2. **Single shared instance across the compare sweep**: one `NRProxyBackend`
   is constructed in `_run_compare` and shared across all per-target
   `make_bisect_predicate` closures. The per-CRF result cache is therefore
   shared across target rungs, which is correct because NR scores are
   target-independent.
3. **Final CRF always gets FR confirmation**: the `cur_lo < cur_hi` guard in
   `bisect_target_vmaf` ensures the last collapsed-window CRF never uses the
   NR path — this preserves the FR correctness guarantee regardless of δ_fast
   tuning.
4. **Sentinel-based coupling**: `_encode_and_score` signals NR skip via a
   structured sentinel string (`__nr_skip__:<direction>;<nr_score>`) in
   `BisectResult.error`, allowing `bisect_target_vmaf` to parse direction +
   NR score without widening the return type.
5. **δ_fast from sidecar JSON**: `calibration_threshold` is written by
   `ai/scripts/calibrate_nr_threshold.py` into `model/tiny/nr_metric_v1.json`
   so the value survives model updates. The compile-time default (8.0 VMAF) is
   used when the sidecar field is absent, matching the ADR-0615 design default.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|--------|------|------|----------------|
| Widen `BisectResult` with an `nr_skipped` bool field | Cleaner API | Changes the frozen dataclass shape; breaks existing callers that pattern-match on fields | Sentinel string avoids the shape change with zero downstream impact |
| Construct `NRProxyBackend` per-predicate | Each predicate has its own cache | Model loaded N times (N = n_codecs × n_targets); GPU EP initialisation adds ~2 s per load | Single shared instance is correct and fast |
| Expose `nr_proxy_backend` in `BisectResult` telemetry fields directly | More observable | Adds a non-serialisable object to the result; breaks JSON export | `fr_calls_total` / `fr_calls_saved` integer fields carry the observable signal without coupling |
| `subprocess.run` the ONNX inference in a separate process | Isolates segfault risk | ~500 ms IPC overhead per call kills the speedup | In-process with error wrapping is the right trade-off at this latency budget |

## Consequences

- **Positive**: 2–4× bisect wall-time reduction on content far from target;
  direct enabler for the DO (ADR-0613) and per-shot ABR (ADR-0614) inner loops.
  Calibration script and report make δ_fast a versioned, auditable artefact.
- **Negative**: Two-model invocation adds ~200 ms overhead on boundary shots
  (NR within δ_fast zone); onnxruntime + numpy become soft dependencies for
  `--fast-nr` (hard error only when the flag is passed).
- **Neutral**: `fr_calls_total` / `fr_calls_saved` telemetry in `BisectResult`
  is always populated when NR is active, zero otherwise — no BC break for
  existing callers.

## References

- ADR-0615: Decision to implement fast NR pre-scoring.
- Research-0611: Design options and calibration plan.
- `tools/vmaf-tune/src/vmaftune/score_backend.py` — `NRProxyBackend` class.
- `tools/vmaf-tune/src/vmaftune/bisect.py` — sentinel path + `make_bisect_predicate`.
- `tools/vmaf-tune/src/vmaftune/cli.py` — `--fast-nr` on `compare` + `tune-per-shot`.
- `tools/vmaf-tune/tests/test_nr_proxy_backend.py` — 40 unit tests.
- `ai/scripts/calibrate_nr_threshold.py` — calibration sweep script.
- `model/tiny/nr_metric_v1.json` — `calibration_threshold` field.
- `docs/usage/vmaf-tune-fast-nr.md` — user-facing documentation.
- Source: per user direction (fast-nr-prescoring re-dispatch, 2026-05-19).
