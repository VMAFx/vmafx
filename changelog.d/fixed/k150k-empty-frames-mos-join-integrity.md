- K150K feature extraction (`ai/scripts/extract_k150k_features.py`)
  no longer silently corrupts the retrain training set on two paths.
  (1) A clip that decodes but scores **zero frames** (vmaf exits 0 with
  an empty frame list) previously aggregated to an all-`NaN` row and was
  still marked done — dropping it from the corpus with no retry;
  `_process_clip` now raises on an empty frame list (honouring its
  documented "raises on any failure" contract) so the clip is logged and
  retried on resume. (2) The MOS-label join keyed the scores CSV
  `video_name` column against `mp4.name`; a filename↔`video_name`
  extension mismatch made **every** label `NaN` with no validation,
  training the regressor on garbage. The join now falls back to the file
  stem, hard-fails the catastrophic zero-coverage case before any GPU
  time is spent, and warns on partial coverage. Two further retrain-path
  guards: the K150K `--limit` flag now prints a runtime NOTE when it
  slices off already-done clips before the resume filter (the documented
  batched-resume footgun), and `ai/train/dataset.py` logs a warning when
  a clip's feature/teacher frame counts diverge by more than two before
  truncating to the shorter (a large gap silently drops/misaligns
  training frames). No golden-data or C-library impact (training harness
  only).
