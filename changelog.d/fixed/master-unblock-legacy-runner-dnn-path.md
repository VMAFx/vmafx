### Fixed

- Add `VmafLegacyQualityRunner` deprecation stub to `compat/python-vmaf/core/quality_runner.py`
  so that `from vmaf.core.quality_runner import VmafLegacyQualityRunner` succeeds (import
  was failing with `ImportError` and blocking the Netflix CPU golden-data CI gate). The class
  raises `NotImplementedError` on instantiation with a pointer to the replacement
  (`VmafQualityRunner`). Removed in ADR-0749 / PR #87. (Unblocks PR #181 Required Checks
  Aggregator.)
- Fix stale post-ADR-0700 `libvmaf/` path references in `ai/` — `ai/tests/test_train_konvid_mos_head.py`
  (`libvmaf/src/dnn/op_allowlist.c` → `core/src/dnn/op_allowlist.c`), and seven
  `libvmaf/build-cpu` → `core/build-cpu` references across `ai/scripts/extract_k150k_features.py`,
  `ai/scripts/konvid_to_vmaf_pairs.py`, `ai/scripts/bvi_dvc_to_full_features.py`,
  `ai/data/feature_extractor.py`, `ai/src/vmaf_train/data/feature_dump.py`,
  `ai/src/vmaf_train/cli.py`, `ai/tests/test_chug_extract_features_smoke.py`, and
  `ai/tests/test_e2e_frame_to_score.py`. Updates corresponding test assertions in
  `ai/tests/test_feature_extractor_defaults.py`.
