- **Metal `integer_adm` kernel.** The Metal backend now implements the
  `integer_adm` extractor (feature `adm` + `adm_scale0..3` — a VMAF default) via
  `integer_adm_metal.mm` + `integer_adm.metal` — a fixed-point 4-scale DWT2 →
  CSF → decouple → contrast-mapping pipeline mirroring the CPU `integer_adm.c`
  arithmetic on the proven `float_adm_metal` multi-scale lifecycle. With this,
  Metal computes the full core VMAF feature set (ADM + VIF + motion) in
  fixed-point — `--backend metal` can now score VMAF on Apple Silicon.
  Parity-checked vs the CPU `adm` on the macOS Apple-Silicon CI lane.
