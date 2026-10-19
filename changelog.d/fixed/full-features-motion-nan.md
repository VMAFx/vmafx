- **`motion` was NaN in every row of every extracted feature table.** libvmaf emits no
  `integer_motion` key; the first-order motion score is `VMAF_integer_feature_motion_sad_score`.
  `ai/data/feature_extractor.py` now reads it, so the `motion` column of `FULL_FEATURES` holds values.
  Tables extracted before this change carry an all-NaN `motion` column and the `verify_features` stage of
  the mini retrain refuses them. `extract_full_features.py` also gains `--assume-dims WxH` for corpora
  that are not 1920x1080.
