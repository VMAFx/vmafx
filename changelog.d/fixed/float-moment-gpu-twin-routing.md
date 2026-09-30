- **`--backend cuda --feature float_moment` now runs `float_moment_cuda`
  instead of the CPU extractor.** The CLI pairs a CPU extractor with its GPU
  twin through the features both declare (ADR-1359), but the CPU
  `float_moment` declared only its own name, `float_moment`, while the
  twins declare the four moments they emit. So the CLI warned that the CUDA
  backend had no twin and computed the feature on the CPU, and the SYCL,
  HIP and Metal twins, which declare the same four names, were missed the
  same way. The CPU extractor now also declares `float_moment_ref1st`,
  `float_moment_dis1st`, `float_moment_ref2nd` and `float_moment_dis2nd`,
  so the lookup finds each backend's twin. On an RTX 4090 the routed run
  equals the CPU at 8 and 10 bpc; at 16 bpc the second moments differ by
  about 7e-6, because the CPU rounds each squared sample in `float`
  (`T-GPU-FLOAT-MOMENT-TWIN-UNREACHABLE-2026-10-01`).
