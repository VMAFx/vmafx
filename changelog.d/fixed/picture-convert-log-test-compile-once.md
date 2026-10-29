- Build: `test_picture_convert_log` links libvmafx's own objects for
  `conversion_context.c`, `conversion_policy.c` and `picture_sample_range.c`
  instead of compiling the three sources a second time. The second compile made
  CodeQL report two used static helpers of `conversion_context.c` as unreachable
  (code-scanning alerts 1589 and 1590).
