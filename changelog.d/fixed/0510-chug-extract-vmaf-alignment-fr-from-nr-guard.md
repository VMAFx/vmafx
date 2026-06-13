- `ai/scripts/extract_k150k_features.py` now refuses to run on a
  full-reference corpus sidecar and points the operator at
  `ai/scripts/chug_extract_features.py` instead (ADR-0510). The
  K150K script is an FR-from-NR adapter (ref == distorted)
  intended for KoNViD-150k-A; running it on a CHUG sidecar
  produced a 5992-row parquet with VMAF~99 across every
  bitrate-ladder rung — including 360p @ 0.2 Mbps which should
  physically score in the 30–60 band — because every clip was
  scored against itself. The new guard detects the FR-corpus
  signature in the `--metadata-jsonl` sidecar (any
  `chug_content_name` group containing both `chug_ref==1` and
  `chug_ref==0` rows) before any worker process is spawned and
  exits 2 with a one-line remediation. Two new regression tests
  on `chug_extract_features.py` pin the FR-aware contract
  (`ref_path != dis_path` for every emitted pair; orphan
  distorted rows without a matching reference are dropped) and
  an end-to-end smoke test on synthetic YUV asserts the ADM
  detail-loss aggregate falls well below the identity-pair
  floor on a deliberately destroyed distorted clip. Bypass via
  `--allow-fr-from-nr` for the rare case of genuine
  identity-pair study on an FR corpus.
