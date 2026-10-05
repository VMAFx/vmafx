- **The arm64 SSIMULACRA 2 kernels meet the HISS-04 size limits.** The 13
  functions over 60 lines in `ssimulacra2_neon.c`, `ssimulacra2_sve2.c` and
  `ssimulacra2_host_neon.c` (the XYB conversion, the SSIM and edge-difference
  maps, both blur passes and the picture-to-linear-RGB conversion) are split
  into static helpers with the same arithmetic and FMA pattern; the SVE2 helpers
  keep their constants as scalars and broadcast them, because SVE vectors cannot
  sit in structs. Scores are byte-identical before and after for NEON and SVE2
  under `qemu-aarch64` on six frame configurations at `--precision max`. The
  HISS baseline loses those 13 infractions.
