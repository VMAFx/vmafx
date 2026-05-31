<!-- markdownlint-disable MD060 -->
# ADR-0646: Route Attached DNN Multi-Output Tensors

- **Status**: Accepted
- **Date**: 2026-05-20
- **Deciders**: Lusoris, Codex
- **Tags**: ai, dnn, api

## Context

ADR-0639 promoted **T-DNN-MULTI-OUTPUT** from an undocumented behaviour to an
open implementation gap: attached tiny models loaded through
`vmaf_use_tiny_model()` / `vmaf_ctx_dnn_attach()` accepted only one scalar ONNX
output per frame. The standalone `vmaf_dnn_session_run()` API already supports
multiple outputs because callers provide explicit output buffers. The attached
path was different: it appended one score into the feature collector under one
feature name, so a graph that emitted `{score, uncertainty}` or several
per-scale scalar heads could run in ORT but could not publish its outputs into a
normal VMAF report.

The fix must preserve the existing single-output collector key. Current users
expect a model whose sidecar name is `fr_regressor_v2` to keep emitting
`fr_regressor_v2`, not `fr_regressor_v2_score`, after a fork upgrade.

## Decision

Attached DNN inference now routes all scalar graph outputs through the existing
feature collector:

- `vmaf_ctx_dnn_run_frame_nchw()` and
  `vmaf_ctx_dnn_run_frame_feature_vector()` call `vmaf_ort_run()` instead of the
  single-output `vmaf_ort_infer()` helper.
- `vmaf_ctx_dnn_attach()` prepares one collector key per ONNX output at attach
  time. Single-output models keep the historical `feature_name` unchanged.
- Multi-output models use sidecar `output_names[]` when the array count matches
  the ONNX output count. Otherwise they use the ONNX graph output names. The
  output suffix is sanitized to `[A-Za-z0-9_]`; duplicate sanitized suffixes
  fall back to deterministic `output<slot>_<attempt>` keys.
- Each attached output tensor must still contain exactly one scalar value. A
  vector or image output tensor is rejected as unsupported by attached mode; the
  standalone DNN session API remains the path for caller-owned tensor outputs.

The sidecar parser accepts `output_names` as an optional string array while
retaining the legacy `output_name` field for old single-output metadata.

## Alternatives considered

| Option | Pros | Cons | Decision |
|---|---|---|---|
| Keep returning `-ENOTSUP` for every `out_n > 1` graph | Smallest code delta | Leaves useful scalar-head models unusable through `--tiny-model` | Rejected |
| Add a new public C API that lets callers provide output-key mappings | Fully explicit and extensible | Public-surface churn, FFmpeg patch churn, and more call-site state for a gap that sidecars already describe | Rejected |
| Flatten vector outputs into `<base>_0`, `<base>_1`, ... | Supports one tensor with many values | Ambiguous for dynamic shapes and makes output count depend on runtime tensor shape | Deferred |
| Use sidecar/ONNX output names for scalar outputs | No public API bump; deterministic report keys; preserves single-output compatibility | Attached mode remains scalar-only | Accepted |

## Consequences

- **Positive**: attached multi-head scalar models can publish every score into
  JSON, XML, and downstream report consumers without a new public API.
- **Positive**: legacy single-output tiny models keep their exact collector key.
- **Negative**: attached mode still does not flatten vectors or image tensors.
- **Neutral / follow-ups**: a future vector-output attach path must define a
  separate naming and shape contract before using the feature collector.

## References

- [ADR-0040](0040-tiny-dnn-feature-extractor.md) — original tiny-DNN attach
  surface.
- [ADR-0518](0518-fr-regressor-v2-codec-aware-runtime.md) — feature-vector tiny
  models and sidecar-driven runtime metadata.
- [ADR-0639](0639-scaffold-audit-p1-feature-plumbing-fixes.md) — documented
  T-DNN-MULTI-OUTPUT as an implementation gap.
- Source: req: "go on with backlog i guess, ai still rolling"
- Source: req: "implement everything that is not blocked by the model"
