<!-- markdownlint-disable MD060 -->
# Research-0725 — AI strict JSONL row helper

## Problem

The strict-JSON sweeps covered manifest/report files, registry updates, and
stdout JSON, but several AI corpus and materializer paths still wrote JSONL
rows with local `json.dumps(row) + "\n"` calls. Those writers were small, but
they repeated serializer policy and could emit non-standard `NaN` / `Infinity`
tokens when exploratory table diagnostics contained non-finite floats.

## Decision

Add `aiutils.jsonl_utils.dumps_jsonl_row()` as the shared strict row serializer
and route non-running AI row emitters through it:

- MOS corpus base ingestion rows
- corpus aggregation and merge outputs
- BVI-DVC corpus rows
- MOS, saliency, and second-opinion materializer rows
- CHUG split/HDR audit sidecars, cache rows, visual-cache rows, and feature rows
- synthetic v3 regressor smoke-corpus rows

The active K150K extractor remains untouched in this PR so the long-running
refresh job is not invalidated mid-run.

## Alternatives considered

| Option | Benefit | Cost | Outcome |
|---|---|---|---|
| Leave row writers local | Smallest diff | Serializer behavior keeps drifting by script | Rejected |
| Use `write_manifest_json()` for JSONL rows | Reuses strict normalization | Wrong abstraction for line-oriented streams | Rejected |
| Add `dumps_jsonl_row()` | One explicit row boundary for all JSONL writers | Small helper and test surface | Chosen |
| Rewrite every remaining JSONL writer, including active K150K | Fastest cleanup | Risky while K150K refresh is running | Deferred |

## Validation

```bash
.venv/bin/python -m pytest ai/tests/test_jsonl_utils.py ai/tests/test_corpus_base.py ai/tests/test_aggregate_corpora.py ai/tests/test_materialize_mos_labels.py ai/tests/test_materialize_saliency_features.py ai/tests/test_second_opinion_features.py ai/tests/test_chug_extract_features_smoke.py::test_chug_split_manifest_uses_strict_json -q
.venv/bin/ruff check ai/src/aiutils/jsonl_utils.py ai/src/aiutils/__init__.py ai/src/corpus/base.py ai/scripts/aggregate_corpora.py ai/scripts/merge_corpora.py ai/scripts/bvi_dvc_to_corpus_jsonl.py ai/scripts/materialize_mos_labels.py ai/scripts/materialize_saliency_features.py ai/scripts/materialize_second_opinion_features.py ai/scripts/chug_extract_features.py ai/scripts/train_fr_regressor_v3.py ai/tests/test_jsonl_utils.py
.venv/bin/black --check ai/src/aiutils/jsonl_utils.py ai/src/aiutils/__init__.py ai/src/corpus/base.py ai/scripts/aggregate_corpora.py ai/scripts/merge_corpora.py ai/scripts/bvi_dvc_to_corpus_jsonl.py ai/scripts/materialize_mos_labels.py ai/scripts/materialize_saliency_features.py ai/scripts/materialize_second_opinion_features.py ai/scripts/chug_extract_features.py ai/scripts/train_fr_regressor_v3.py ai/tests/test_jsonl_utils.py
```
