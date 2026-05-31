<!-- markdownlint-disable MD013 MD060 -->
# ADR-0039: Pull forward runtime op-allowlist walk and model registry

- **Status**: Accepted
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ai, dnn, security, supply-chain

## Context

Audit on 2026-04-17 found that `core/src/dnn/op_allowlist.c::vmaf_dnn_op_allowed` existed with unit-test coverage but was never called at model load time from `ort_backend.c::vmaf_ort_open`. The comment in `model_loader.c:200-202` claiming "deep op-allowlist walk is done by ort_backend.c" was aspirational — any `.onnx` containing disallowed ops (`Loop`, `If`, custom ops) loaded successfully via `--tiny-model`. In parallel, ADR-0036 queues 7+ `.onnx` files across Wave 1 sub-tracks with no source-of-truth manifest. Both gaps must close before the Wave 1 smoke PR ships any positive-path inference.

## Decision

Land in the Wave 1 smoke PR: (1) minimal ONNX protobuf-wire-format scanner at `core/src/dnn/onnx_scan.c` — parses `ModelProto.graph` (field 7) → `GraphProto.node` (field 1, repeated) → `NodeProto.op_type` (field 4) with strict bounds-checking; (2) called from `vmaf_dnn_validate_onnx` before `CreateSession`; (3) `realpath` + symlink-escape hardening in the same entry point; (4) `model/tiny/registry.json` schema v0 with SHA-256 per entry, populated via `vmaf-train register`; (5) end-to-end CI smoke gate loading a generated 1KB `smoke_v0.onnx` via the full `vmaf --tiny-model` path.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Leave allowlist Python-side only | Already done at export | Real threat is untrusted `--tiny-model` path into shipped binary | Rejected |
| Add `libprotobuf-c` as meson subproject | Full protobuf support | Disproportionate dependency surface for one field scan; widens TCB | Rejected |
| Use ORT's public graph-introspection | Leverages existing runtime | ONNX Runtime 1.22 C API does not expose per-node op-type iteration in stable surface | Rejected |
| Defer walk to follow-up, ship smoke first | Unblocks smoke PR | Widens attack surface before negative-path gate in place | Rejected |
| Field-scan + registry now (chosen) | ~150 LOC; auditable; bounds-checked; TCB tiny; registry schema anchors Wave 1 | Must hand-roll a trivial protobuf scanner | Correct — smallest safe wedge |

## Consequences

- **Positive**: untrusted `.onnx` rejected at load time with bounded CPU cost; registry gives Wave 1 a single source of truth.
- **Negative**: must maintain the 150-LOC scanner in sync with ONNX spec; registry schema v0 locks in before more models pile on.
- **Neutral / follow-ups**: Sigstore-sign the registry entries (ADR-0010 flow).

## References

- Source: `req` (user: "Start the smoke-PR (Recommended)" — popup following roadmap-agent plan flagging the runtime op-walk gap)
- Related ADRs: ADR-0022, ADR-0036, ADR-0040
