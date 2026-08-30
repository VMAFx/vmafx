# Research-0696: Full-Feature Exporter Manifests

## Problem

The refreshed AI training tables are only useful evidence when an operator can
replay how they were produced. ADR-0668 already moved the K150k extractor,
combiner, and metadata enricher to sibling manifest sidecars, but the KoNViD
and BVI-DVC full-feature parquet builders still emitted anonymous local
parquet files.

That gap matters now because refreshed Netflix, KoNViD, BVI-DVC, CHUG, and UGC
tables are being combined repeatedly for model refreshes. A parquet without
the source root, model, vmaf binary, cache, feature order, and row counts is
too weak to use as training evidence after the original shell history is gone.

## Finding

`konvid_to_full_features.py` and `bvi_dvc_to_full_features.py` already own the
right semantic boundary: they choose the source material, encode the distorted
side, run libvmaf with the current `FULL_FEATURES` tuple, and write the per-frame
table. The missing piece is a sibling JSON manifest, not a schema change to the
parquet rows.

The shared ADR-0661 helper is sufficient. It normalizes CLI arguments, records
path existence and hashes where possible, and keeps JSON ordering stable for
tests and future diffs.

## Decision Drivers

- Keep training-table rows unchanged so existing trainers and combiners keep
  loading old and new parquets.
- Make each full-feature shard self-describing before it is combined into
  multi-corpus training tables.
- Record corpus-specific context that the generic combiner cannot infer later:
  KoNViD fold settings, BVI-DVC input mode/tier, feature order, cache path, and
  selected clip counts.
- Use `aiutils.run_manifest` rather than inventing another sidecar schema.

## Implementation Notes

Both exporters now accept `--manifest-out`. When omitted, the default is the
output parquet path with `.manifest.json` suffix:

- `runs/full_features_konvid.manifest.json`
- `runs/full_features_bvi_dvc_<tier>.manifest.json`

The manifests include the feature list, CRF/codec recipe, cache policy, row and
clip counters, output paths, and an ADR-0661 `run_provenance` block. KoNViD also
records folded-output settings; BVI-DVC records `--bvi-zip` versus `--bvi-dir`,
the selected tier, and extractor group list.

Unit tests run both scripts with mocked libvmaf/ffprobe calls and assert the
manifest schema, counters, and shared provenance block.

## Follow-Up

When the refreshed full-feature shards are regenerated for model training, keep
the manifest sidecars with the local parquet artifacts and cite them in any
model card that consumes the table.
