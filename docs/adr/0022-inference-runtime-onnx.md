<!-- markdownlint-disable MD013 MD060 -->
# ADR-0022: Inference runtime is ONNX Runtime via execution providers

- **Status**: Accepted
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ai, dnn, cuda, sycl, build

## Context

The fork already ships CPU / CUDA / SYCL / HIP backends. Tiny-AI inference needs a runtime that spans the same matrix without forking inference code per backend, and must be safely sandboxable with an operator allowlist.

## Decision

We will use ONNX Runtime C API; gate the build with Meson `-Denable_dnn=auto`; ORT execution providers map to libvmaf backends — CPU → CPU EP, CUDA → CUDA EP, SYCL/Intel → OpenVINO EP, HIP → ROCm EP.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| TensorRT | Fast on NVIDIA | NVIDIA-only; no CPU/SYCL/HIP path | Splits inference code per backend |
| OpenVINO direct | Great on Intel | Intel-focused; no CUDA/HIP | Same |
| Per-backend native runtimes | Max perf | Huge duplication; each backend diverges | Too expensive |
| ONNX Runtime (chosen) | One runtime; execution providers match our backend matrix 1:1; sandboxed graph | Some perf overhead vs native | Rationale: avoids forking inference paths; op allowlist restricts further |

## Consequences

- **Positive**: single inference code path; execution provider selection mirrors backend selection.
- **Negative**: ORT pulls in transitive deps; must maintain op allowlist.
- **Neutral / follow-ups**: ADR-0039 wires the runtime op-allowlist + registry.

## References

- Source: `Q5.3`
- Related ADRs: ADR-0020, ADR-0021, ADR-0023, ADR-0039
