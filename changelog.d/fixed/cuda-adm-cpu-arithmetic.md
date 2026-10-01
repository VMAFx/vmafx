- **`adm_cuda` returns the CPU's scores bit for bit.** The CUDA twin of the
  fixed-point ADM extractor carried its own copy of the CSF weight routine,
  which multiplied the exponent in `float` where the CPU multiplies in
  `double`, so its weights were 1 to 3 units in the last place off and
  `integer_adm_scale1..3`, `adm2` and `adm3` up to 2.1e-7 from the CPU. Its
  denominator kernels rounded each warp of a row where the CPU rounds the
  row, which shows on frames with little reference detail (6.6e-7), and
  derived the scale-0 rounding shift from an fp32 logarithm: on frames whose
  scale-0 border region has an area just above a power of two (81 areas up
  to 2^26, for example 962x13542) the denominator came out twice too large
  and `integer_adm_scale0` up to 0.12 too low. The twin now takes its CSF
  weights, rounding shifts and score conclusion from the CPU's own routines
  and folds the denominator once per row
  ([ADR-1416](docs/adr/1416-cuda-adm-cpu-row-rounding.md)). Measured on an
  RTX 4090 at `--precision max`: every output of every frame identical on
  the Netflix pair at 8, 10, 12 and 16 bits, both 1080p checkerboard pairs
  and BBB 3840x2160, with `debug=true` and with every option, including
  `adm_csf_mode` 1 to 3 and `adm_skip_scale0`. The parity gate compares this
  twin with tolerance 0. No change in time per frame (3.63 and 3.66 ms at
  3840x2160). Stored `adm_cuda` outputs change by up to 2.1e-7, and on the
  frame sizes above by the amounts given. The CPU extractor's scores do not
  change.
