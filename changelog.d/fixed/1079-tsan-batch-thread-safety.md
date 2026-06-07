# test(core): add TSan-eligible thread-safety test for threaded_extract_batch_func

Added `core/test/test_thread_safety_batch.c` to provide TSan coverage for the
`threaded_extract_batch_func` PREV_REF lifecycle fix (ADR-1072, PR #765) and the
`flush_context_threaded` `is_initialized` gate fix (ADR-1073, PR #769).
`test_pic_preallocation.c` was excluded from all sanitizer runs due to
huge-alloc SIGABRT in the TSan allocator; the new test covers the same code paths
using `vmaf_picture_alloc` (64×64 frames, no pool) and is not in any exclusion
list. (ADR-1079)
