<!-- markdownlint-disable MD060 -->
# ADR-0395: predictor stub-models policy

- **Status**: Accepted
- **Date**: 2026-05-08
- **Deciders**: Lusoris
- **Tags**: ai, vmaf-tune, predictor, models, fork-local

## Context

The per-shot VMAF predictor (`tools/vmaf-tune/src/vmaftune/predictor.py`,
PR #430) loads one ONNX model per codec adapter at runtime. Without
shipped artefacts, every fresh checkout falls back to the analytical
curve and `Predictor(model_path=...)` either raises `FileNotFoundError`
or downstream tests skip the ONNX path entirely. CI cannot pin
"the ONNX model loads" without a shipped binary; documentation cannot
demonstrate the predict-then-verify flow without a model file to
point at.

A "real" per-codec predictor needs a corpus of ~thousands of
`(features, codec, crf, real_vmaf)` rows captured from a representative
source set — probe encodes, signalstats, saliency, real VMAF measured
with libvmaf — and that corpus is generated on the operator's machine
via `vmaftune.corpus`. It is not something CI can produce on demand.

## Decision

We will ship a synthetic-stub ONNX model for each of the 14 codec
adapters under `model/predictor_<codec>.onnx`, trained from a
deterministic 100-row synthetic corpus seeded by the codec name. The
trainer (`tools/vmaf-tune/src/vmaftune/predictor_train.py`) accepts a
real Phase A corpus via `--corpus path/to/file.jsonl` and falls back to
the synthetic generator on a per-codec basis when the corpus is absent
or contains zero rows for that codec. Every shipped model card carries
`corpus.kind: synthetic-stub-N=100` and a prominent
"do-not-use-in-production" warning. The runtime predictor surface is
unchanged; consumers do not need to know which kind of model they
loaded.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Ship synthetic stub per codec (chosen) | Reproducible, byte-stable; CI can pin the load + monotonicity; one-line operator override (`--corpus real.jsonl`). | Predictions track the analytical fallback, not learned signal. Card warning mandatory. | Best trade-off: CI gets a real ONNX file to load, predictor PR's `model_path=` branch becomes testable, and the cost when the operator wants real weights is exactly the corpus run they were going to do anyway. |
| Ship one shared stub for all 14 codecs | One file to sign, smaller repo footprint. | Hides per-codec coefficient differences (codec-specific `_DEFAULT_COEFFS`); operator gets identical predictions across codecs and a misleading impression of fitted weights. | Erases the per-codec contract the runtime predictor depends on. |
| Ship no model files; require operators to train | Zero risk of stub being mistaken for production. | `Predictor(model_path=...)` is dead code on a fresh checkout; CI can never test the ONNX branch; documentation has no working example. | Drops the predict-then-verify integration test surface. |
| Ship a real corpus + real models | Honest predictions out of the box. | Real corpus run = multi-day encode sweep on the operator's hardware; not feasible for a fork-local PR. Production weights flip is a separate workstream gated on real corpus generation. | Out of scope for this PR; tracked as the production-weights follow-up. |

## Consequences

- **Positive**: `Predictor(model_path=...)` and `pick_crf` ONNX paths
  become unit-testable; CI gates the load + clamping + monotonicity per
  shipped model. The trainer pipeline (synthetic-corpus → ONNX →
  op-allowlist check → model card) is exercised on every PR. Operators
  with a real corpus run one command to flip every codec to real weights.
- **Negative**: Future contributors must read the model card before
  trusting predictions. Stubs add ~300 KB to the repo (14 × ~22 KB).
  PLCC / SROCC reported in stub cards are artificially high (the
  synthetic target *is* the analytical fallback, so the network smooths
  itself); the warning section flags this.
- **Neutral / follow-ups**: When a real corpus lands, re-run the trainer
  and bump the registry / commit the new ONNX bytes. The card writer
  drops the synthetic-stub warning automatically based on the
  `corpus.kind` field.

## References

- [`docs/ai/predictor.md`](../ai/predictor.md) — runtime + training
  documentation (5-point bar per ADR-0042).
- [ADR-0042](0042-tinyai-docs-required-per-pr.md) — per-PR doc bar for
  tiny-AI surfaces.
- [ADR-0237](0237-quality-aware-encode-automation.md) — vmaf-tune Phase A
  corpus.
- [ADR-0276](0276-vmaf-tune-phase-d-per-shot.md) — per-shot CRF
  scaffold the predictor feeds.
- [`tools/vmaf-tune/src/vmaftune/predictor_train.py`](../../tools/vmaf-tune/src/vmaftune/predictor_train.py)
  — trainer.
- [`tools/vmaf-tune/src/vmaftune/predictor.py`](../../tools/vmaf-tune/src/vmaftune/predictor.py)
  — runtime consumer.
- Source: `req` — task brief specifies "synthetic 100-row training corpus
  per codec, deterministic seed".
