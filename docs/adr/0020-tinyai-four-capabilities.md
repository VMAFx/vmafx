<!-- markdownlint-disable MD013 MD060 -->
# ADR-0020: Tiny-AI scope covers all four capabilities

- **Status**: Accepted
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ai, dnn, framework, cli

## Context

The tiny-AI program had four candidate capabilities: (C1) custom VMAF FR quality models, (C2) learned no-reference metrics, (C3) learned filters for encoders, (C4) LLM-based dev helpers. Narrowing scope was possible, but the four partition cleanly — C1/C2/C3 share the ONNX Runtime inference path, and C4 is dev-only tooling that does not link into libvmaf.

## Decision

We will include all four tiny-AI capabilities: C1 custom VMAF FR quality models, C2 learned no-reference metrics, C3 learned filters for encoders, C4 LLM dev helpers.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| C1 only | Smallest scope | Leaves NR and encoder-side unaddressed | Narrow scope rejected |
| C1+C2 only | Inference-side only | No encoder-side gain | Same |
| All four (chosen) | Shared infra; C4 isolated under `dev-llm/` | Larger implementation | Partitions cleanly |

Rationale note: the four capabilities partition cleanly (C1/C2/C3 share the inference runtime; C4 is dev-only), so "all four" doesn't multiply complexity. C4 explicitly does NOT link into libvmaf — it lives in `dev-llm/` and only surfaces through `.claude/skills/`.

## Consequences

- **Positive**: inference infrastructure is designed once for three scoring-side uses; dev-helper LLM surfaces are isolated.
- **Negative**: large surface; Wave 1 scope (ADR-0036) must carefully stage deliverables.
- **Neutral / follow-ups**: ADR-0021, ADR-0022, ADR-0023 lock training stack, runtime, surfaces.

## References

- Source: `Q5.1`
- Related ADRs: ADR-0021, ADR-0022, ADR-0023, ADR-0036
