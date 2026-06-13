- macOS CI tests: complete the ADR-0749 ansnr-feature sunset by removing the
  20 residual `VMAF_feature_anpsnr_score` / `VMAF_integer_feature_anpsnr_score`
  assertions from `feature_extractor_test.py` (the float-ansnr C feature emits
  both `ansnr` AND `anpsnr` keys — PR #276 cleared the `ansnr` half but missed
  the `anpsnr` half) and skipping 5 tests that depend on resource fixtures or
  SVM models that hard-require the legacy `ansnr` feature
  (`feature_param_sample{,_with_optional_dict,_with_optional_dict_good}.py` and
  `model/other_models/nflx_v1.json`). Unblocks 4 macOS CI jobs on master tip.
