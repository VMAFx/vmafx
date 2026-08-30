- **`vmaf-tune` corpus schema v2 → v3 — canonical-6 per-feature
  aggregates land as first-class columns.** Adds 12 new corpus row
  fields (`adm2_mean`, `vif_scale[0..3]_mean`, `motion2_mean` plus
  matching `_std` counterparts) parsed straight from libvmaf's
  `pooled_metrics.<feature>` block. Bumps `SCHEMA_VERSION` from 2 to
  3. Missing features (cambi-only models, encode failures) land as
  `NaN` — never `0.0` — so trainers drop the row instead of fitting
  on synthetic zeros. The reader (`vmaftune.corpus.read_jsonl`)
  back-fills the new columns on legacy v2 rows with `NaN`, so older
  corpora stay loadable. Unblocks `train_fr_regressor_v2.py` and
  `train_fr_regressor_v3.py` consuming canonical-6 features directly
  from the corpus DataFrame; v3 retires its synthetic-by-default
  fallback on the real-corpus path. See ADR-0366 + the partial-
  integration audit at `docs/research/0091-partial-integration-audit-
  2026-05-08.md`.
