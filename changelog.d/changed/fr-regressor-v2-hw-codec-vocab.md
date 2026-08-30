- **`fr_regressor_v2` ENCODER_VOCAB extended with hardware encoders.**
  Adds `h264_nvenc`, `hevc_nvenc`, `av1_nvenc`, `h264_qsv`, `hevc_qsv`,
  `av1_qsv` to the closed encoder vocabulary used by codec-aware
  training (`ai/scripts/train_fr_regressor_v2.py`). Bumps
  `ENCODER_VOCAB_VERSION` from 1 to 2. The accompanying
  `PRESET_ORDINAL` table gains entries for NVENC's `p1..p7`
  preset family and Intel QSV's libx264-aligned vocab. Validated on
  a 216-row real corpus (9 Netflix sources × 6 hardware codecs × 4
  CQ values, aggregated from 33,840 per-frame rows produced by
  `scripts/dev/hw_encoder_corpus.py`): PLCC 0.96 / SROCC 0.95 /
  RMSE 4.15 in-sample. Pre-extension training on the same corpus
  gave PLCC 0.92 / RMSE 6.41 (all hw codecs collapsing to
  `unknown`); the vocab extension is the lift.
