- **arXiv-style tech-note draft for the production-flip gate and conformal-VQA novelty
  claims (Research-0090).** Lands a DRAFT preprint at
  [`docs/research/0090-arxiv-techNote-prodflip-conformal-2026-05-09.md`](../docs/research/0090-arxiv-techNote-prodflip-conformal-2026-05-09.md)
  covering the two patterns flagged as "no clear public prior art" by the SOTA digest
  ([Research-0086](../docs/research/0086-tiny-ai-sota-deep-dive-2026-05-08.md), PR #449):
  the [ADR-0303](../docs/adr/0303-fr-regressor-v2-ensemble-prod-flip.md) two-criterion
  ensemble production-flip gate (mean LOSO PLCC + max-min spread; verdict and
  PROMOTE.json from PR #423), and the [ADR-0279](../docs/adr/0279-fr-regressor-v2-probabilistic.md)
  conformal-VQA prediction surface (split-conformal + CV+; coverage probe `0.9515`
  vs `0.95` nominal pinned by `tools/vmaf-tune/tests/test_conformal.py` from PR #488).
  Research-and-writing only — no code changes; the draft is the deliverable. Format
  is Markdown for `pandoc` conversion to LaTeX when the user opts to submit.
