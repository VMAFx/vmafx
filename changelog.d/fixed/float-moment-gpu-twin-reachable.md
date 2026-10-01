- **`--backend <gpu> --feature float_moment` runs the backend's twin.** The
  command computed `float_moment` on the CPU and warned that the backend had
  no twin, although `float_moment_cuda`, `float_moment_sycl`,
  `float_moment_hip` and `float_moment_metal` exist: the CPU extractor
  declared the pseudo-name `float_moment` instead of the four features it
  writes, so the lookup that pairs an extractor with its twin never matched.
  It now declares `float_moment_ref1st`, `float_moment_dis1st`,
  `float_moment_ref2nd` and `float_moment_dis2nd`. Naming the twin and the
  CPU extractor together (`--feature float_moment_cuda --feature
  float_moment`) no longer fails with `feature "float_moment_ref1st" cannot
  be overwritten`. Scores were correct before
  and are unchanged on the CPU; on an RTX 4090 the twin equals the CPU on
  every frame at 8, 10 and 12 bits and is within 7e-6 on the second moments
  at 16 bits. See [Float moment](docs/metrics/features.md).
