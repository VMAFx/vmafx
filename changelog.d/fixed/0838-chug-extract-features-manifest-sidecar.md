`chug_extract_features.py` now writes a `<output>.manifest.json` replay
sidecar (schema `chug-feature-extraction-manifest-v1`) after each extraction
run, per ADR-0668. The sidecar carries `run_provenance`, `written_rows`, and
`feature_set`. Pass `--manifest-out` to redirect it. Previously the script
embedded provenance only in the optional `--split-manifest` and
`--audit-output` sidecars, leaving the feature JSONL itself anonymous.
