---
paths:
  - ai/scripts/mini_retrain.py
  - ai/src/aiutils/pipeline.py
  - ai/src/aiutils/mini_corpus.py
  - ai/src/aiutils/retrain_checks.py
  - ai/e2e/test_mini_retrain_e2e.py
  - .github/workflows/mini-retrain.yml
invariant: retrain tooling runs end to end in CI; each stage has manifest; killed run resumes; NaN fails gate.
---
<!-- markdownlint-disable MD013 MD060 -->
# Mini retrain and the stage runner

- [ADR-1898](../../docs/adr/1898-mini-retrain-pipeline.md) — **script retrain runs is stage of `mini_retrain.py` with runbook's flags.** Adding trainer, exporter or evaluator to `docs/ai/retrain-runbook-1246.md` means adding its stage (inputs, outputs, `stable` set, check) to `build_stages()` and planted-defect case to `ai/e2e/test_mini_retrain_e2e.py` in same PR. `aiutils.pipeline.run_pipeline()` resumes only on `complete` manifest with unchanged key and intact outputs; never write stage that updates its input in place (use `Stage.copies`). `aiutils.retrain_checks.gate_verdict()` fails NaN metric; do not replace it with `plcc < gate`, which passes NaN. `ai/e2e/` is not under 60 s `ai/tests` limit and is run by Tiny AI job and nightly.
