- **Metal `integer_vif` kernel.** The Metal backend now implements the
  `integer_vif` extractor (feature `vif` — a VMAF default) via
  `integer_vif_metal.mm` + `integer_vif.metal` — a 4-scale fixed-point Gaussian
  pyramid with int64 moment accumulators and the integer log2-LUT, mirroring the
  proven `float_vif_metal` scaffold and the CPU `integer_vif.c` arithmetic.
  Parity-checked vs the CPU `vif` on the macOS Apple-Silicon CI lane.
  `--backend metal --feature vif` now scores on Apple Silicon — together with
  `integer_ssim` / `float_adm` / `float_vif` this brings Metal close to full
  VMAF-scoring parity.
