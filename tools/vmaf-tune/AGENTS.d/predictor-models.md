---
paths:
  - tools/vmaf-tune/src/vmaftune/predictor*.py
  - tools/vmaf-tune/tests/test_predictor*.py
invariant: Predictor stub models: 3-step retrain, commit new bytes, refresh card; not for production CRF picks.
---
<!-- markdownlint-disable MD024 -->
# Predictor models retrain policy

## Predictor stub-models policy (ADR-0395)

Fork ships one `model/predictor_<codec>.onnx` per codec adapter.
As of 2026-05-14 NVENC / QSV predictors (`h264_nvenc`,
`hevc_nvenc`, `av1_nvenc`, `h264_qsv`, `hevc_qsv`, `av1_qsv`) are
real-corpus retrains from `runs/phase_a/full_grid/comprehensive.jsonl`
and their cards carry `corpus.kind: real-N=<rows>`. Software and
AMF predictors remain synthetic stubs until matching real corpora
exist. Trainer
(`ai/src/vmaf_train/predictor_train.py`, moved out of vmaf-tune so no
runtime package declares torch, ADR-1886) sources its
`CODECS` tuple from `predictor._DEFAULT_COEFFS` so two stay
single-source. When new codec adapter is added (e.g. future
`vp9_qsv` row in `_DEFAULT_COEFFS`), same PR must:

1. Re-run `python3 -m vmaf_train.predictor_train --output-dir model`
   in the ai/ environment
   to produce matching `predictor_<codec>.onnx` + card.
2. Commit new ONNX bytes — shipped-model smoke test
   parameterises over `CODECS` and fails if coefficient row has
   no shipped artefact.
3. Refresh model card's `corpus.kind` line on every retrain
   (trainer does this automatically; review diff).

Stub models are explicitly **not** for production CRF picks.
Synthetic target *is* analytical fallback, so PLCC / SROCC numbers
in stub cards are artificially high. Real-corpus retrains follow
same trainer entry point with `--corpus path/to/file.jsonl`
or `--corpus path/to/corpus-dir/` and produce honest metrics.
Directory corpus inputs are recursive and sorted so
`.corpus/corpus_run/` trains deterministically without
manual concatenation step. Keep that directory handling reachable
from both `train_all_codecs()` and CLI; file-only `is_file()`
guards above `load_corpus()` silently turn real corpus
directories back into synthetic stubs. Loader accepts both
canonical `encoder` / `crf` / `vmaf_score` /
`bitrate_kbps` rows and historical hardware-sweep `codec` / `q` /
`vmaf` / `actual_kbps` aliases; do not reintroduce external
conversion scripts for those local corpora.
