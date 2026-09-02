- **Metal `float_adm` kernel.** The Metal backend now implements the `float_adm`
  extractor (`float_adm2`, `adm_scale0..3`, `aim_score`, `adm3_score`) via
  `float_adm_metal.mm` + `float_adm.metal` — a 1:1 port of the CUDA
  `float_adm/float_adm_score.cu` 4-scale DWT2 → CSF → decouple → contrast-mapping
  pipeline (plus the AIM second pass), parity-checked vs the CPU `float_adm` on
  the macOS Apple-Silicon CI lane. `--backend metal --feature float_adm` now
  scores on Apple Silicon — Metal can now compute both core VMAF features
  (ADM + VIF) in floating-point.
