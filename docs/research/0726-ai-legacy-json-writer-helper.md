<!-- markdownlint-disable MD060 -->
# Research-0726 — AI legacy strict JSON writers

## Problem

After the manifest, stdout, registry, and JSONL sweeps, a few legacy AI
artifact paths still used local `json.dumps()` / `Path.write_text()` calls for
durable JSON files. Those paths are not new schemas, but they are still
operator-visible artifacts:

- Netflix tiny-AI per-clip caches.
- KoNViD/BVI per-clip libvmaf feature caches.
- The legacy `ai.train.eval.EvalReport` JSON report.
- YouTube-UGC content manifests.
- K150K metadata-enrichment stdout summaries.

Local serializers make non-finite diagnostics script-dependent. Python's
default JSON encoder can emit `NaN` / `Infinity`, which is not valid RFC-8259
JSON and breaks strict dashboards, notebooks, and release tooling.

## Decision

Reuse `aiutils.run_manifest.write_manifest_json()` and
`dumps_manifest_json()` for legacy JSON artifacts that keep their existing
schema but need the same strict serialization boundary as run manifests.

The helper now accepts any JSON-like payload, including list-shaped cache
documents, so old per-clip caches can keep their reader contract while replacing
non-finite values with `null`.

## Alternatives considered

| Option | Benefit | Cost | Outcome |
|---|---|---|---|
| Leave legacy writers local | Smallest diff | Non-standard JSON can still leak through caches/reports | Rejected |
| Add a second helper for cache JSON | More semantic naming | Duplicates the existing strict-normalization boundary | Rejected |
| Reuse `write_manifest_json()` for reports and caches | One strict JSON implementation; no schema churn | Helper name is broader than manifests in legacy paths | Chosen |
| Rewrite all remaining cache paths, including active K150K | Maximum cleanup | Risky while the K150K refresh job is running | Deferred |

## Validation

```bash
.venv/bin/python -m pytest ai/tests/test_run_manifest.py ai/tests/test_eval.py ai/tests/test_netflix_loader.py ai/tests/test_legacy_corpus_extraction_manifests.py ai/tests/test_konvid_full_features.py ai/tests/test_bvi_dvc_dir_mode.py ai/tests/test_dataset_fetch_manifests.py ai/tests/test_enrich_k150k_parquet_metadata.py -q
.venv/bin/ruff check ai/src/aiutils/run_manifest.py ai/data/netflix_loader.py ai/train/eval.py ai/scripts/extract_full_features.py ai/scripts/konvid_to_vmaf_pairs.py ai/scripts/konvid_to_full_features.py ai/scripts/bvi_dvc_to_full_features.py ai/scripts/fetch_youtube_ugc_subset.py ai/scripts/enrich_k150k_parquet_metadata.py ai/tests/test_run_manifest.py ai/tests/test_eval.py ai/tests/test_netflix_loader.py ai/tests/test_legacy_corpus_extraction_manifests.py ai/tests/test_konvid_full_features.py ai/tests/test_bvi_dvc_dir_mode.py
.venv/bin/black --check ai/src/aiutils/run_manifest.py ai/data/netflix_loader.py ai/train/eval.py ai/scripts/extract_full_features.py ai/scripts/konvid_to_vmaf_pairs.py ai/scripts/konvid_to_full_features.py ai/scripts/bvi_dvc_to_full_features.py ai/scripts/fetch_youtube_ugc_subset.py ai/scripts/enrich_k150k_parquet_metadata.py ai/tests/test_run_manifest.py ai/tests/test_eval.py ai/tests/test_netflix_loader.py ai/tests/test_legacy_corpus_extraction_manifests.py ai/tests/test_konvid_full_features.py ai/tests/test_bvi_dvc_dir_mode.py
```
