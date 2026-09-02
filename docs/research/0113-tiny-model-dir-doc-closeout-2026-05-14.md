# Research-0113: Tiny-model directory jail documentation closeout

- **Status**: Active
- **Workstream**: docs-only closeout for `VMAF_TINY_MODEL_DIR`
- **Last updated**: 2026-05-14

## Question

Do the public tiny-AI docs still describe `VMAF_TINY_MODEL_DIR` as a future
security feature after the loader and tests already enforce it?

## Sources

- [`core/src/dnn/model_loader.c`](../../core/src/dnn/model_loader.c) —
  `enforce_tiny_model_jail()` and the `getenv("VMAF_TINY_MODEL_DIR")` call in
  `vmaf_dnn_validate_onnx`.
- [`core/test/dnn/test_model_loader.c`](../../core/test/dnn/test_model_loader.c)
  — regression coverage for unset jail, allowed in-jail model, outside model,
  sibling-prefix escape, symlink escape, missing jail, non-directory jail, and
  trailing-slash normalisation.
- [`docs/ai/security.md`](../ai/security.md) — stale text still labelled the
  environment variable as "Planned (not yet implemented)".

## Findings

The implementation is already present and is stronger than the stale docs
claimed. `vmaf_dnn_validate_onnx()` resolves the model path first, caches the
environment variable once for the call, applies the jail before normal model
file validation, and returns `-EACCES` for every jail violation. The test suite
covers the important path-boundary cases: no-jail compatibility, valid in-jail
loads, outside paths, prefix confusion, symlink escapes, and misconfigured jail
values.

The public docs were the weak point. `docs/ai/security.md` still said the env
var was not implemented, while `docs/ai/inference.md` and
`docs/ai/model-registry.md` did not tell operators how the path jail relates to
`--tiny-model` and `--tiny-model-verify`.

## Alternatives explored

Leaving the docs untouched was rejected because it hides an already-available
deployment hardening knob and contradicts the code. Adding a new ADR was also
rejected: this PR does not introduce a new policy or implementation decision,
it corrects stale user-facing documentation for behaviour that is already
implemented and tested.

## Open questions

- None for this closeout. Future work may add a CLI-visible diagnostic for
  jail failures, but the current `-EACCES` behaviour is already load-bearing.

## Related

- Docs: [`docs/ai/security.md`](../ai/security.md),
  [`docs/ai/inference.md`](../ai/inference.md),
  [`docs/ai/model-registry.md`](../ai/model-registry.md)
