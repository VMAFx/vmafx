- **Metal `float_vif` kernel.** The Metal backend now implements the `float_vif`
  extractor (`float_vif` + `float_vif_scale0..3`) via `float_vif_metal.mm` +
  `float_vif.metal` — a 4-scale separable-Gaussian pyramid with per-scale
  mean/variance/covariance statistics, ported from the CUDA / CPU `float_vif`
  references and parity-checked vs the CPU reference on the macOS Apple-Silicon
  CI lane. Brings the Metal backend a step closer to full VMAF-scoring parity
  (`--backend metal --feature float_vif` now scores on Apple Silicon).
