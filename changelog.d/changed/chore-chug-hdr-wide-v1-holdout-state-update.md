- **`docs/state.md`**: add dedicated `T-CHUG-HDR-WIDE-V1-HOLDOUT-VALIDATION`
  row recording the 2026-05-27 held-out test result (PLCC 0.8468 / SROCC 0.8188 /
  RMSE 0.2639, gate FAIL by narrow margin). Update `T-MOS-HEAD-PRODFLIP` to
  cross-reference the new row instead of carrying stale CHUG trainer instructions.
  Reproducer: `python ai/scripts/validate_chug_hdr_mos_head.py`. ADR-0687.
