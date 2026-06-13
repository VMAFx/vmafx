# Research-0115: fr_regressor_v2 model-card status closeout

- **Status**: Active
- **Workstream**: docs-only closeout for `fr_regressor_v2` model cards
- **Last updated**: 2026-05-14

## Question

Do the `fr_regressor_v2` model cards still describe the model as scaffolded or
planned after the registry has promoted the shipped ONNX to a production
`smoke: false` entry?

## Sources

- [`model/tiny/registry.json`](../../model/tiny/registry.json) — current row
  for `fr_regressor_v2`, including `smoke: false`, SHA-256 pin, and production
  notes.
- [`docs/ai/models/fr_regressor_v2.md`](../ai/models/fr_regressor_v2.md) —
  stale scaffold-status card.
- [`docs/ai/models/fr_regressor_v2_codec_aware.md`](../ai/models/fr_regressor_v2_codec_aware.md)
  — older ADR-0235-era planned card for a separate codec-aware graph.
- [`docs/ai/models/fr_regressor_v3.md`](../ai/models/fr_regressor_v3.md) —
  production successor using the 16-slot encoder vocabulary.

## Findings

`fr_regressor_v2` is no longer scaffold-only. The registry marks
`fr_regressor_v2.onnx` as a production checkpoint (`smoke: false`) trained on
the vmaf-tune Phase-A JSONL corpus, with an in-sample PLCC of 0.9794 and a
stable SHA-256 pin. The main v2 card still described the smoke-mode training
path as the shipped artefact, and the older `fr_regressor_v2_codec_aware` card
still implied that a separate `fr_regressor_v2_codec_aware.onnx` was the next
planned output.

The docs should instead distinguish the shipped vmaf-tune v2 graph from the
historical ADR-0235 design. The older card remains useful as design history,
but its status needs to be "superseded" so operators do not look for a model
file that is intentionally not in the registry.

## Alternatives explored

Deleting the older codec-aware card was rejected because ADR-0235 and older
research digests still link to it. Keeping it but marking it superseded keeps
that audit trail intact while making the live production path unambiguous.

Adding a new ADR was rejected because this PR does not change model policy,
model bytes, registry schema, or runtime behaviour; it only aligns public docs
with already-shipped registry state.

## Open questions

- None for this closeout.

## Related

- Docs: [`fr_regressor_v2.md`](../ai/models/fr_regressor_v2.md),
  [`fr_regressor_v2_codec_aware.md`](../ai/models/fr_regressor_v2_codec_aware.md),
  [`fr_regressor_v3.md`](../ai/models/fr_regressor_v3.md)
