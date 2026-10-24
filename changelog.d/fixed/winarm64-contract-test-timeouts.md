- **Two host-compiler contract tests no longer time out on Windows ARM64.**
  `test_vmafx_rgb_math_contract` timed out at 61 s on the Windows ARM64 MSVC
  job and `test_stale_text_contract` took 112.5 s, because the Go generator
  tests ran beside them on the same cores; seven clean runs took 6.2-14.4 s and
  10.7-23.0 s. Both now run alone (`is_parallel : false`), with limits of 2x
  the slowest clean run (28 s and 46 s), so a real slowdown still fails.
