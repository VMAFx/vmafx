Remove 14 dead `print_128_*` / `print_256_*` debug-print macros (~86 lines) from
`core/src/feature/x86/motion_avx2.c`. The macros were never called in production
code, had no `#include <stdio.h>` guard, and were flagged by the AVX-512 audit as
dead code violating the ADR-0141 touched-file lint-clean rule.
