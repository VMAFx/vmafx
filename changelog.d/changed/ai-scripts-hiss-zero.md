- **`ai/scripts` is at the HISS standard.** The long drivers of the corpus,
  feature-extraction, calibration and trainer scripts (`aggregate_corpora`,
  `calibrate_phase_f_recipes`, `chug_extract_features`, `extract_k150k_features`,
  `materialize_*`, `batch_materialize_*`, `bvi_dvc_to_full_features`,
  `train_konvid_mos_head`, `train_fr_regressor`, `train_predictor_v2_realcorpus`
  and others) are split into helpers that keep their public names and
  signatures; the three manifest CSV parsers share `corpus.base.parse_mos_stats`,
  and `train_konvid_mos_head` trains its fold and ship models through one
  `_fit_model` (same seed, same RNG draw order, same synthetic corpus). Library
  code no longer ends the process: `collect_gpu_calibration_data.write_parquet`
  raises `ParquetUnavailableError` (the command still exits 2 with the same
  message) and the two placeholder exporters return their status from `main()`
  (exit 1 with `missing <registry>` unchanged). No score or manifest changes;
  the HISS baseline loses the rows of these files.
