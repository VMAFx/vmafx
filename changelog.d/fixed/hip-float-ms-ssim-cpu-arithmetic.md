- **`float_ms_ssim` on the HIP backend is bit-identical to the CPU.**
  `integer_ms_ssim_hip` accumulated its decimation and its Gaussian window
  sums as fp32 running sums, divided fp64 numerators by fp64 denominators and
  combined unrounded per-scale means, where the CPU extractor fuses each
  decimation tap, adds fp32 products in fp64, divides by fp32 denominators and
  combines fp32 means. On a gfx1036 no frame matched the CPU: the score was up
  to 3.0e-6 off and a per-scale mean up to 2.1e-5. The kernels now follow the
  CPU extractor operation for operation, as the CUDA twin does since ADR-1403,
  and every value of every frame is the CPU's at `--precision max`: the
  Netflix pair at 8 and 10 bits, both 1080p checkerboard pairs and BBB
  3840x2160, `enable_lcs`, `enable_db` and `clip_db` outputs included. The
  exact window sums cost time on that device: about 37 instead of 30 ms per
  1920x1080 frame and 169 instead of 158 ms per 3840x2160 frame. Re-run
  any stored HIP `float_ms_ssim` output
  ([HIP backend](docs/backends/hip/overview.md#integer_ms_ssim_hip)).
