- **CUDA: `aim` and `adm3` sub-features** — `float_adm_cuda` now emits
  `VMAF_feature_aim_score` and `VMAF_feature_adm3_score` via two new
  CUDA kernel stages (2b `float_adm_csf_r`, 3b `float_adm_aim_cm`).
  `--backend cuda` no longer silently falls back to CPU for HDR VMAF
  model features; parity with CPU held to `places=4`.
  Six new options match the CPU `float_adm` extractor defaults:
  `adm_bypass_cm`, `adm_adm3_apply_hm`, `adm_p_norm`, `adm_dlm_weight`,
  `adm_min_val`, `adm_skip_aim_scale`.
  (ADR-0574)
