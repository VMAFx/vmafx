- **Two host-compiler contract tests no longer time out on Windows ARM64.**
  `test_vmafx_rgb_math_contract` (limit 60 s, normally 6-14 s) timed out at
  61 s on the Windows ARM64 MSVC job while the Go generator tests ran beside
  it, and `test_stale_text_contract` took 112.5 s of its 120 s there. Their
  Meson limits are now 240 s and 300 s, with the measured times in
  `core/test/meson.build`.
