- **CHUG sidecar `chug_bit_depth` keep-list behavior restored (BUG-048 Sec A4)**:
  Restores `chug_bit_depth` in `_load_jsonl_metadata` within `ai/scripts/extract_k150k_features.py`
  which was landed in `be10f1906` (#1137) and silently reverted in `dce84d442` (#981).
  Ensures 10-bit CHUG clips are inferred as `yuv420p10le` rather than defaulting to `yuv420p`.
  Covered by regression test `test_geometry_from_sidecar_infers_10bit_pix_fmt` in
  `ai/tests/test_extract_k150k_features.py`.
