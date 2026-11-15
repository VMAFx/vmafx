---
paths:
  - core/src/feature/ssim.c
  - core/src/feature/ssim.h
  - core/src/feature/float_ssim.c
invariant: Integer SSIM samplemax² widening, fork-side registration, and pthread_once dispatch.
area: ssim-and-metrics
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Integer SSIM Widening, Registration, and Dispatch

- **integer SSIM `samplemax²` must be widened to `double` (16-bpc
  parity, 2026-06-27)**: `integer_ssim.c` `ssim_reduce_row_range`
  computes `c1/c2 = sm * sm * K * w_d²` via hoisted
  `const double sm = (double)samplemax;`. It must NOT regress to
  `int samplemax * samplemax` form: for 16-bpc `samplemax = 65535` and
  `65535² > INT_MAX` overflows `int` (UB, wraps negative), corrupting
  stability constants and diverging from CUDA/HIP/SYCL twins
  (which already use `int64_t`/`double`). 8/10/12-bpc are bit-unchanged
  by cast. Guarded by `test_ssim_16bit_distorted_in_range` in
  `core/test/test_ssim_coverage.c` (fails against `int` form).

## `vmaf_fex_ssim` is registered fork-side, not upstream

Upstream Netflix's `feature_extractor.c` does **not** list
`&vmaf_fex_ssim` in its `feature_extractor_list[]`, and upstream
does not compile `integer_ssim.c` either — both are dormant on
upstream master branch. fork wires both paths up so that
`vmaf --feature ssim` resolves at CLI;
fix-up touches three upstream-mirror surfaces (registry-array
row in `feature_extractor.c`, matching `extern` declaration,
and `#include "config.h"` in `integer_ssim.c`) plus one
fork-local meson-build line. **On every upstream sync, re-check
that fork's three additions remain in place.** If upstream
ever lands its own integer-SSIM registration, drop fork's row
in favour of upstream's; file structure is identical so
diff should resolve cleanly in `git rebase`. `config.h` include
in `integer_ssim.c` is load-bearing on Vulkan-enabled LTO builds —
without it `VmafFeatureExtractor` struct layout disagrees
between TUs (different `HAVE_CUDA` / `HAVE_SYCL` / `HAVE_VULKAN`
visibility) and GCC fires `-Wlto-type-mismatch` at link time.
generated `config.h` include in `feature_extractor.h` is
project-wide guard for this layout; keep it there so every extractor
definition and every registry consumer sees same backend fields.

## SSIM SIMD dispatch globals are pthread_once-installed (ADR-0871)

`float_ssim.c` and `float_ms_ssim.c` install SSIM/iqa_convolve
SIMD dispatch tables (`g_ssim_precompute`, `g_ssim_variance`,
`g_ssim_accumulate`, `g_iqa_convolve` in `iqa/ssim_tools.c`) by
calling `iqa_ssim_install_dispatch_once(&s_dispatch_guard,
installer_cb)` from their per-extractor `init()`. Guard
parameter is retained for API symmetry only. Actual mutual
exclusion is provided by file-static `pthread_once_t` inside
`iqa/ssim_tools.c`, shared across both TUs. Without that sharing,
two per-TU guards would each be allowed to fire once, racing
on same dispatch globals (TSan-confirmed 2026-05-30).

**Invariants** when modifying SSIM-related dispatch code:

- Never call `iqa_ssim_set_dispatch` / `iqa_convolve_set_dispatch`
  directly from per-extractor `init()` body. setters
  themselves are unsynchronised by design; only correct
  serialisation point is `iqa_ssim_install_dispatch_once`.
- installer callbacks across TUs must remain idempotent (install
  same ISA-best function pointers). If future SIMD path
  (e.g. SVE, AVX10) is added, extend existing installer rather
  than creating parallel one — once-guard fires exactly once
  process-wide.
- If future upstream commit refactors `float_ssim::init` or
  `float_ms_ssim::init`, preserve
  `iqa_ssim_install_dispatch_once(...)` call. Replacing it with
  bare dispatch install resurrects data race.

See [ADR-0871](../../../../docs/adr/0871-ssim-dispatch-pthread-once.md)
and underlying research digest at
[`docs/research/tsan-race-audit-2026-05-30.md`](../../../../docs/research/tsan-race-audit-2026-05-30.md).
