- `core/test/test.h` (`mu_report`): missing `\n` after the red
  `fail` label caused the failure summary line to print on the same
  terminal row. Newline added.
- `core/test/test.c` (`main`): stray leading `, ` in the failure
  summary `fprintf` format string removed.
- `core/test/test.h`: added `#ifndef LIBVMAF_TEST_H` include guard,
  resolving the double-include risk noted in ADR-0245 and removing
  the mandatory include-order constraint for `simd_bitexact_test.h`.
