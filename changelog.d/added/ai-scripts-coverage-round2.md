### test(ai/scripts): coverage push round 2 — argv + helpers + happy paths

Added 129 new unit tests in `ai/tests/test_scripts_coverage_round2.py` covering
helper functions and argv parsing for the highest-LOC `ai/scripts/` targets:

- `extract_k150k_features.py` — geometry sidecar parsing, FPS parsing, HDR
  detection, motion FPS weight, `_feature_arg` option filtering, metric alias
  lookup, frame aggregation (NaN suppression), staging JSONL roundtrip,
  checkpoint helpers, content split determinism, parquet dedup, vmaf cmd
  builder, merge frame metrics.
- `calibrate_nr_threshold.py` — linear regression, delta_fast (2σ), Pearson r,
  calibration quality gate, YUV geometry detection, pixel-format/bitdepth
  mapping, and CLI argument validation.
- `materialize_mos_labels.py` — key normalisation modes (raw/basename/stem/auto),
  MOS payload edge cases, column auto-detection, table format support (CSV, JSONL,
  JSON list/rows-dict), extra-name normalisation, CLI `--key-normalize` flag.
- `batch_materialize_saliency_features.py` — manifest loading error paths,
  Markdown report generation, `BatchRunOptions` defaults.
- `batch_materialize_second_opinion_features.py` — manifest loading error paths,
  score-spec resolution (with/without label prefix), Markdown report generation,
  `SecondOpinionBatchOptions` defaults.
- `aggregate_corpora.py` — CLI multi-input end-to-end, `--corpus-source-override`
  flag, `transform_row` per-corpus identity paths, `_resolve_corpus_source` logic.

All tests run without GPU, corpus downloads, or model artifacts.
