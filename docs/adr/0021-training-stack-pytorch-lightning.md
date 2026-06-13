<!-- markdownlint-disable MD013 MD060 -->
# ADR-0021: Training stack is PyTorch + Lightning with ONNX export

- **Status**: Accepted
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ai, python, framework

## Context

Tiny-AI training needs a framework that ships models into ONNX Runtime (ADR-0022) and gives reasonable defaults for DDP, mixed precision, and checkpointing. The candidate frameworks each have trade-offs.

## Decision

We will use PyTorch + Lightning (Python package at `ai/`); export to ONNX via `torch.onnx.export` opset 17+; roundtrip-validate exports against onnxruntime to `atol=1e-5`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| TensorFlow / Keras | Mature | Two ML frameworks in dev tree; ONNX export second-class | Rejected per rationale |
| JAX | Fast; functional | Second-class ONNX export | Rejected |
| ggml | Tiny runtime | Cannot train | Rejected |
| PyTorch + Lightning (chosen) | Mainstream; mature ONNX; free DDP/AMP/ckpt | Another framework to learn | Rationale: "mainstream, mature ONNX export, Lightning gives free DDP / mixed-precision / checkpointing" |

## Consequences

- **Positive**: one training framework for all tiny-AI models; exports validated bit-close to native inference.
- **Negative**: PyTorch install footprint is large; opset drift (see ADR-0041) requires registry schema flexibility.
- **Neutral / follow-ups**: ADR-0022 picks the inference runtime.

## References

- Source: `Q5.2`
- Related ADRs: ADR-0020, ADR-0022, ADR-0041
