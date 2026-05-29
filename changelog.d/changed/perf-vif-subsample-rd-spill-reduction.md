### Performance

- **`vif_subsample_rd_8_avx512` register-spill elimination (ADR-0503)**: extracted
  the vertical and horizontal inner j-loop bodies into `__attribute__((noinline))`
  helpers, reducing the simultaneous ZMM live-set from ~30 to ~20 per helper.
  `objdump` confirms 56 → 0 ZMM stack-spill stores. Expected ~3–5% improvement on
  integer VIF throughput for 8-bit 1080p content. Bit-exactness preserved and
  verified by `test_vif_simd` + Netflix golden gate (132 assertions).
