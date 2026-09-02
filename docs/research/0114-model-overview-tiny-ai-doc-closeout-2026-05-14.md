# Research-0114: Model overview tiny-AI documentation closeout

- **Status**: Active
- **Workstream**: docs-only closeout for shipped tiny-AI model docs
- **Last updated**: 2026-05-14

## Question

Do the human-facing model overview and FAQ still claim that tiny-AI model
weights are future work after `model/tiny/registry.json` already ships
production and smoke ONNX artefacts?

## Sources

- [`docs/models/overview.md`](../models/overview.md) — stale
  "Tiny-AI models (planned)" section.
- [`docs/reference/faq.md`](../reference/faq.md) — stale FAQ answer saying
  no first-milestone weights ship yet.
- [`model/tiny/registry.json`](../../model/tiny/registry.json) — current
  source of truth for shipped tiny-AI ONNX entries, their `kind`, and their
  `smoke` production/CI role.
- [`docs/ai/overview.md`](../ai/overview.md),
  [`docs/ai/inference.md`](../ai/inference.md), and
  [`docs/ai/model-registry.md`](../ai/model-registry.md) — canonical tiny-AI
  operator docs.

## Findings

The stale text was contradicted by the registry. Current master ships
production entries for VMAF-tiny regressors, codec-aware FR regressors,
LPIPS-SqueezeNet, saliency students, TransNet V2, and learned filters,
plus explicit smoke-only rows for CI and compatibility. The public model
overview should point readers at those families without turning into a
second registry, and the FAQ should show a real invocation using a shipped
model ID rather than a future `vmaf_tiny_vN` placeholder.

## Alternatives explored

Duplicating every registry row into `docs/models/overview.md` was rejected:
it would drift again as soon as a model is added or promoted. The chosen
shape keeps a family-level table in the overview and directs operators to
`model/tiny/registry.json` plus per-model cards for authoritative details.

Adding a new ADR was rejected because this PR does not introduce a new
model policy or runtime surface; it corrects stale public prose for
artefacts that are already shipped and documented elsewhere.

## Open questions

- None for this closeout.

## Related

- Docs: [`docs/models/overview.md`](../models/overview.md),
  [`docs/reference/faq.md`](../reference/faq.md),
  [`docs/ai/model-registry.md`](../ai/model-registry.md)
