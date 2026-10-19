---
paths:
  - docs/ai/training-data.md
  - docs/ai/models/*.md
  - model/tiny/registry.json
  - scripts/ci/tests/test_model_card_dataset_terms.py
invariant: Card of model on outside data quotes data's terms verbatim from training-data.md; test holds it.
---
<!-- markdownlint-disable MD013 -->
# Model cards quote their training data's terms (ADR-1570)

Shipped tiny models trained on Netflix Public, KoNViD-1k, BVI-DVC, DUTS-TR or
ImageNet data: card has `## Training data terms` = verbatim quotes of each
dataset's terms + fork's reading (stated as fork's, not permission) + RC9
retrain row `T-TINY-AI-RETRAIN-CLEARED-DATA-2026-10-04`. Canonical quotes live
once in `docs/ai/training-data.md#dataset-terms` (one `###` per dataset,
blockquotes only for dataset's own words). Never paraphrase dataset's
terms in card; never edit quote in one card only. New model on outside
data, or retrain that changes data: add or update `###` section,
card section, and `CARDS` in
`scripts/ci/tests/test_model_card_dataset_terms.py` (fails on missing
section or quote that differs from canonical text).
