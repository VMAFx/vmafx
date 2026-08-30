# Tiny-AI Per-PR Doc Bar

[ADR-0042](../adr/0042-tinyai-docs-required-per-pr.md) sets a tighter
**5-point doc bar** for any PR touching the tiny-AI surface (`ai/`,
`core/src/dnn/`, anything model-card-bearing). It is the
specialisation of the project-wide
[ADR-0100](../adr/0100-project-wide-doc-substance-rule.md) doc-
substance rule for tiny-AI.

The 5 points (per ADR-0042) — **paraphrased**, see the ADR for the
authoritative wording:

1. A model card under [`docs/ai/models/`](models/) or an updated
   training-data / inference / quantisation page.
2. Reproducer command in the PR description.
3. PLCC / SROCC / RMSE reading on a held-out fold.
4. ONNX-graph operator-allowlist conformance check.
5. CHANGELOG entry under `changelog.d/added/` or `changelog.d/changed/`.

This bar co-exists with — does not replace — the project-wide r10
rule from [`CLAUDE.md` §12](../../CLAUDE.md). PRs that touch *both*
a tiny-AI surface and a non-tiny-AI surface satisfy both bars
independently.

## Applies To

- Model files under `model/tiny/`.
- Model cards under `docs/ai/models/`.
- Training, export, quantisation, or calibration scripts under `ai/`.
- Runtime inference paths under `core/src/dnn/`.
- Registry, model-verification, or tiny-device selector changes.

## Practical Checklist

Before opening a tiny-AI PR, make sure the PR contains:

1. The model card or topic page that explains how a human should use
   the changed artefact.
2. The training or export command that can reproduce the artefact.
3. The held-out metrics that justify promotion, or an explicit
   `smoke: true` / deferred status when no promotion is claimed.
4. An ONNX allowlist or runtime compatibility check when a graph
   changes.
5. A changelog fragment in the appropriate `changelog.d/` section.

ADRs and code comments do not substitute for the user-facing model card.

## See also

- [ADR-0042](../adr/0042-tinyai-docs-required-per-pr.md) — the
  policy itself.
- [ADR-0100](../adr/0100-project-wide-doc-substance-rule.md) — the
  project-wide rule this specialises.
- [`docs/ai/overview.md`](overview.md) — tiny-AI surface overview.
