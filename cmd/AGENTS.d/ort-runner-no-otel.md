---
paths:
  - cmd/vmafx-ort-runner/*.go
  - pkg/ai/*.go
invariant: vmafx-ort-runner stays OTel-free; inference span owned by caller pkg/ai.Registry.Infer.
---
<!-- markdownlint-disable MD013 MD029 -->
# `vmafx-ort-runner` stays OTel-free (ADR-1134)

## Rebase-sensitive invariants

5. **`vmafx-ort-runner` stays OTel-free** (ADR-1134, its AGENTS.md #5).
   Inference span belongs to caller (`pkg/ai.Registry.Infer`); do not "fix"
   runner by adding init to it.
