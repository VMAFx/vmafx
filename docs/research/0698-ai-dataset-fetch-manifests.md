# Research-0698: AI Dataset Fetch Manifests

## Question

Which remaining dataset fetch helpers still create operator-local AI inputs
without durable replay evidence before the corpus conversion / feature-table
steps?

## Findings

The current provenance chain covers training reports, derived feature tables,
corpus JSONL merge/aggregate outputs, legacy trainer-input builders, and MOS
source adapters. The two fetch helpers predate that policy:

- `fetch_konvid_1k.py` downloads and extracts KoNViD-1k archives, then leaves
  only the local directory shape as evidence.
- `fetch_youtube_ugc_subset.py` writes a stem-to-file content manifest for the
  selected VP9 compressed UGC 4-tuples, but not the run-level selection policy,
  source bucket URL, argv, or output bundle.

Both scripts are used before later generated JSONL/parquet artifacts can record
their own inputs. A later trainer can prove that it consumed a corpus root, but
not how that root was fetched or whether a small subset, full dataset, or stale
partial rerun produced it.

## Decision Drivers

- Keep existing data manifests stable for downstream consumers.
- Reuse ADR-0661 `run_provenance` rather than inventing fetch-specific path
  schemas.
- Make the sidecars automatic, with explicit override flags for dated bundles.
- Keep tests network-free by stubbing download/list/extract helpers.

## Implementation Notes

`fetch_konvid_1k.py` now writes `<root>/fetch_manifest.json` by default and
accepts `--manifest-out`. The manifest records archive URLs, size sanity floors,
archive paths/sizes, extracted-directory status, `--keep-zips`, and
`run_provenance`.

`fetch_youtube_ugc_subset.py` keeps `--manifest` as the content manifest and
adds `--run-manifest-out`, defaulting to `<manifest>.run-manifest.json`. The run
manifest records the GCS bucket prefix, smallest-complete-4tuple selection
policy, selected stems/files/sizes, output paths, and `run_provenance`.

## Follow-Up

Regenerate local fetch manifests before citing KoNViD-1k or the YouTube-UGC
subset in promoted model cards. Full-corpus YouTube UGC ingestion already has a
separate adapter replay manifest; this change only closes the standalone subset
fetch helper used by vmaf_tiny_v5 / UGC expansion experiments.
