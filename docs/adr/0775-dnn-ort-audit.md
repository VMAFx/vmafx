<!-- markdownlint-disable MD013 MD060 -->
# ADR-0775: DNN ORT Backend Audit Findings

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: dnn, onnx, ort, thread-safety, correctness, fork-local, research

## Context

An audit of `core/src/dnn/` was requested to assess memory safety, thread-safety
guarantees, provider-selection correctness, model cache lifetime, and ORT API error
path coverage. No changes were made; the digest is Research-0775. This ADR records
the findings and follow-up work items.

## Decision

Accept Research-0775 findings as the authoritative audit. No code changes in this PR
(diagnosis only). Three follow-up items are filed:

1. Document the per-session thread-safety contract in `dnn.h` (medium-severity latent
   race if callers ever dispatch `vmaf_read_pictures` concurrently).
2. Fix the `VMAF_DNN_DEVICE_AUTO` chain to try OpenVINO:CPU after OpenVINO:GPU fails,
   matching the comment at `ort_backend.c:275`.
3. Propagate `GetTensorElementType` failure instead of silently leaving element type
   as `ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED`.

## Alternatives considered

| Option | Decision |
|---|---|
| Fix inline (audit + fix in one PR) | Rejected — the user explicitly requested audit only; fixes will land as targeted PRs so reviewers can see each change in isolation |
| Skip ADR (trivial audit) | Rejected — thread-safety gap and provider-chain mismatch are non-trivial findings requiring tracked remediation items |

## Consequences

**Positive:**

- Establishes a clear baseline: the ORT integration is largely correct; only the three
  items above need follow-up.
- Documents that `vmaf_read_pictures` must not be called concurrently against the same
  `VmafContext` when a tiny model is attached, until a mutex is added.

**Negative / follow-up:**

- Three open issues (documented above) require separate PRs.

**Neutral:**

- Current production call pattern (single-threaded `vmaf_read_pictures`) is safe; the
  latent race is not triggered today.

## References

- Research digest: [`docs/research/research-0775-dnn-ort-backend-audit.md`](../research/research-0775-dnn-ort-backend-audit.md)
- `core/src/dnn/ort_backend.c` — session open/close/run
- `core/src/dnn/dnn_api.c` — `VmafDnnSession` lifecycle
- `core/src/libvmaf.c` — `VmafContext.dnn` field and `vmaf_ctx_dnn_run_frame`
- ADR-0113 — two-stage `CreateSession` CPU fallback
- ADR-0517 — feature-vector model input rank
