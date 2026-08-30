Fix missing `#include <stddef.h>` in `core/src/feature/x86/cambi_avx512.c`;
`ptrdiff_t` used in `calculate_c_values_row_avx512` was not declared in
sanitizer builds (address / undefined / thread), causing a compile failure at
step 40/663 introduced by PR #907.
