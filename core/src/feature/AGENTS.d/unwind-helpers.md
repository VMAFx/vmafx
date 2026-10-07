---
paths:
  - core/src/feature/ciede.c
  - core/src/feature/feature_collector.cpp
invariant: Error exits use unwind helpers instead of goto labels; cleanup order is invariant.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Error Exits Unwind Helpers and Cleanup Order

## Error exits use unwind helpers, not label ladders (HISS-01, 2026-09-21)

Cleanup `goto` is gone from `ciede`, `feature_collector`, `feature_dists`,
`feature_lpips`, `float_moment`, `float_ms_ssim`, `float_psnr`, `float_ssim`,
`motion` and `pu21`. Pattern that replaced it: one `static` unwind helper per
constructor, in same translation unit, releasing resources in same order old
`free_*:` chain used. Shallow exits reach same helper; members not yet acquired
are still NULL, and `free(NULL)` is no-op. Where release set cannot be inferred
from NULL (feature-collector mutex and aggregate vector, pu21 buffer pair):
helper takes explicit stage enum and releases every stage at or below it,
highest first.

Two rules for anyone adding error path here:

- Do not reintroduce `goto`. Add case to unwind helper instead.
- Free order is behaviour. Enumerate exit paths before and after any edit
  and check them against each other. `pthread_mutex_destroy` before
  `free(fc)`, `aligned_free` in acquisition-reverse order, `vmaf_picture_unref`
  before scratch release.
