<!-- markdownlint-disable MD013 -->
# Research-0046: `vmaf-tune` saliency-aware ROI tuning (Bucket #2)

- **Status**: Companion to [ADR-0293](../adr/0293-vmaf-tune-saliency-aware.md)
- **Date**: 2026-05-03
- **Tags**: tooling, ai, saliency, ffmpeg, codec, fork-local

## Question

Bucket #2 of the PR #354 audit calls for saliency-aware ROI tuning
in `vmaf-tune`. The fork already ships:

- `saliency_student_v1.onnx` (PR #359, BSD-3-Clause-Plus-Patent,
  ~113K params, fork-trained on DUTS-TR) — the model.
- `vmaf-roi` C sidecar (ADR-0247) — emits per-CTU QP-offset files
  for x265 / SVT-AV1, one frame per invocation.
- `vmaf-tune` Python harness (ADR-0237 Phase A, PR #329) — drives
  ffmpeg, scores with libvmaf, emits JSONL.

The design question is: **how should `vmaf-tune` consume the
saliency signal — by re-implementing inference in Python, or by
shelling out to `vmaf-roi`?**

## Option space

### Option A — pure-Python ONNX inference (chosen)

`saliency.py` loads `saliency_student_v1.onnx` via `onnxruntime`,
samples N frames from the source YUV, runs inference, averages
into one mask, maps to QP offsets, reduces to per-MB, writes an
x264 `--qpfile`.

- Pro: zero binary dependency on a built libvmaf — `vmaf-tune` is
  a pip-installable Python package; pulling in a built C binary
  for every recommend invocation is friction.
- Pro: clean test-seam (`session_factory=…`); the test suite runs
  without onnxruntime *or* ffmpeg installed.
- Pro: Python is the natural layer for x264 qpfile orchestration;
  C-side `vmaf-roi` writes one file per frame and would need a
  Python-side aggregator anyway.
- Con: numeric duplication of the saliency→QP signal blend (~5
  numpy lines vs ~25 C lines — bit-for-bit equivalent, pinned by
  one test).
- Con: requires `onnxruntime` for the saliency code path. Mitigated
  by graceful fallback to plain encode when the import fails.

### Option B — shell out to `vmaf-roi`

Drive `vmaf-roi` once per sampled frame, parse its output, average,
write the qpfile.

- Pro: one source of truth for the signal blend — no duplication
  with the C code.
- Pro: covers x265 / SVT-AV1 sidecar formats for free.
- Con: requires a built libvmaf tree on PATH at runtime (`vmaf-roi`
  is built when `-Denable_tools=true`); `vmaf-tune` becomes harder
  to ship as a standalone Python package.
- Con: per-frame subprocess overhead (~50 ms each on a typical
  CPU) and per-frame YUV re-decode.
- Con: `vmaf-roi` is one-frame-per-invocation today (Wave 1); a
  per-clip aggregate needs Python-side aggregation regardless.

### Option C — bake into `corpus`

Add a `saliency_aware: bool` row column and let the grid sweep
emit saliency-aware encodes alongside plain ones.

- Pro: one subcommand, no new CLI surface.
- Con: conflates "sweep a grid" (corpus) with "produce a
  recommended encode" (recommend); downstream Phase B (target-VMAF
  bisect) and Phase C (per-shot CRF predictor) would have to
  special-case saliency-aware rows.

## Decision

Option A. The graceful-fallback posture covers the
no-onnxruntime case so callers always get a result; the bit-for-bit
contract with `vmaf-roi`'s C signal blend is pinned by
`test_saliency_to_qp_map_neutral_is_zero` +
`test_saliency_to_qp_map_high_saliency_lowers_qp`. x265 / SVT-AV1
extension is a one-file follow-up under `codec_adapters/` once a
qpfile formatter for those encoders lands; until then those
codecs route through `vmaf-roi` directly.

## Open follow-ups

- Per-frame ROI (rather than per-clip aggregate) once `vmaf-tune`
  decodes per-frame.
- x265 / SVT-AV1 codec adapters with native qpfile / ROI-map
  emitters.
- Real-binary integration coverage (ffmpeg + onnxruntime + real
  YUVs) gated behind a `--integration` pytest mark; tracked
  alongside Phase B.
- Bitrate-savings empirical sweep (Pareto curve over
  `--saliency-offset` ∈ {-2, -4, -6, -8}) — lands with Phase B.

## References

- [ADR-0293](../adr/0293-vmaf-tune-saliency-aware.md) — this decision.
- [ADR-0237](../adr/0237-quality-aware-encode-automation.md) — `vmaf-tune` umbrella.
- [ADR-0247](../adr/0247-vmaf-roi-tool.md) — `vmaf-roi` sidecar (signal blend).
- [ADR-0286](../adr/0286-saliency-student-fork-trained-on-duts.md) — `saliency_student_v1` (PR #359).
