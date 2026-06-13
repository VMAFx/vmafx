- **Header self-containment**: Added missing `#include <stddef.h>` to `thread_pool.h`
  (uses `size_t`), `integer_vif.h` (uses `ptrdiff_t`), and
  `feature/cuda/integer_vif_cuda.h` (uses `ptrdiff_t`); added `#include <cstdint>` to
  `feature/cuda/integer_adm/adm_decouple_inline.cuh` (uses `uint16_t`, `int8_t`,
  `int32_t`, `int64_t`). Mechanical follow-up to PR #918 header-self-contain rule.
