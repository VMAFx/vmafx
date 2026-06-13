- **MOS-label batch manifests for KonViD and CHUG** (`ai/configs/`):
  `mos-label-batch-konvid.json` wires the KonViD-1k and KonViD-150k
  feature parquets to their corpus JSONL label files using the conventional
  6+ digit numeric key regex; `mos-label-batch-chug.json` joins CHUG
  UGC-HDR features using raw `chug_video_id` keys. Run via
  `python ai/scripts/batch_materialize_mos_labels.py --manifest ai/configs/mos-label-batch-konvid.json`
  (ADR-0992).
- **Smoke tests** (`ai/tests/test_mos_label_batch_runs_smoke.py`): validate
  manifest JSON schema, corpus-specific key-column and normalisation policy,
  and end-to-end batch runs with synthetic data (no corpus files required on
  CI). Fixes a pre-existing `sys.path` bug in
  `ai/tests/test_batch_materialize_mos_labels.py` that prevented the tests
  from running when pytest was invoked from the repo root (ADR-0992).
