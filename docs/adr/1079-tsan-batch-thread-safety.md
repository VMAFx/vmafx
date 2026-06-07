<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1079: TSan-eligible thread-safety test for threaded_extract_batch_func

- **Status**: Accepted
- **Date**: 2026-06-06
- **Deciders**: Lusoris
- **Tags**: `ci`, `test`, `threading`

## Context

PRs #765 (ADR-1072) and #769 (ADR-1073) fixed two concurrency bugs in
`threaded_extract_batch_func` (libvmaf.c):

1. **ADR-1072**: bare `memset` on `fex->prev_ref` after `extract()` without
   calling `vmaf_picture_unref` first; exhausted the picture pool after
   ~pool_size frames and deadlocked `pthread_cond_wait`.

2. **ADR-1073**: `flush_context_threaded` called `fex->flush()` on the shared,
   never-initialized extractor context. `flush()` lazily allocated
   `s->feature_name_dict`; `vmaf_feature_extractor_context_close` returned
   `-EINVAL` early (guarded by `is_initialized == false`) and leaked the dict.
   Fix: set `fex_ctx->is_initialized = true` immediately before the flush loop,
   after `vmaf_thread_pool_wait` has already serialized all workers.

The existing `test_pic_preallocation.c` exercises these paths (via
`vmaf_preallocate_pictures` + `vmaf_fetch_preallocated_picture`) but is excluded
from every sanitizer run — ASan, UBSan, and TSan — because
`vmaf_preallocate_pictures` allocates large pool buffers (up to 40 × 1920×1080 ≈
95 MB) that the TSan shadow-memory allocator cannot satisfy at its default
reservation limit, triggering SIGABRT before any test logic runs. The exclusion
covers the entire test binary, including the threading sub-tests that have no
huge-alloc dependency.

This leaves the post-PR `threaded_extract_batch_func` PREV_REF code path —
specifically the `vmaf_picture_unref` + `memset` sequence and the `is_initialized`
write — with no TSan coverage. A latent data race that escapes the
`happens-before` edges established by `vmaf_thread_pool_wait` would be
undetectable until a production deadlock or ASan report surfaced it.

## Decision

Add `core/test/test_thread_safety_batch.c`, a TSan-eligible regression test that
covers the same `threaded_extract_batch_func` + `flush_context_threaded` code
paths using `vmaf_picture_alloc` (heap allocation, no pool) and small (64×64)
frames. The test is registered in `core/test/meson.build` as a `fast`-suite
target, is NOT added to any sanitizer exclusion list, and therefore automatically
participates in the TSan run in `sanitizers.yml`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Fix `test_pic_preallocation.c` to avoid huge allocs | Single file, no new file | The pool-based tests are the point of that file; removing the pool would defeat them. The TSan crash is inherent to `vmaf_preallocate_pictures` at large pool sizes. | Structural conflict between pool-size stress and sanitizer allocator limits. |
| Split `test_pic_preallocation.c` into pool and non-pool halves | Keeps all preallocation tests together | Changes an existing excluded file and risks re-triggering the SIGABRT on new sub-tests if the split is imperfect. | New file is safer: zero risk of accidentally reintroducing huge-alloc patterns. |
| Guard huge-alloc sub-tests with `#if __has_feature(thread_sanitizer)` | Single file | Requires TSan-detection macros in every affected sub-test; fragile across compilers; masks the sub-test from the sanitizer view rather than running a clean variant. | More fragile than a dedicated file that is clean by construction. |
| Do nothing (accept the coverage gap) | No new code | The ADR-1072/ADR-1073 fixes have zero TSan visibility; any regression in the PREV_REF lifecycle or is_initialized guard would be silent until a deadlock surfaces in production. | Unacceptable for two fixes that directly address data-race-adjacent lifetime bugs. |

## Consequences

- **Positive**: `threaded_extract_batch_func` PREV_REF lifecycle (ADR-1072) and
  `flush_context_threaded` `is_initialized` gate (ADR-1073) now have TSan
  coverage on every master push. Regressions in either fix surface as TSan
  data-race reports within the 35-minute sanitizer budget.
- **Positive**: The test uses `vmaf_picture_alloc` + 64×64 frames; TSan shadow
  overhead is trivial (~1 MB). No new sanitizer exclusion entry is needed.
- **Negative**: One additional test binary in the `fast` suite; compile time
  increase is negligible (single `.c` file linked against existing `libvmaf`).
- **Neutral**: `test_pic_preallocation.c` remains excluded from all sanitizer
  runs; its pool-stress tests (e.g. `test_picture_pool_stress` with 16 threads
  and a 4-picture pool) are not covered by the new test, which is intentional —
  that test stresses pool exhaustion, not the PREV_REF lifecycle.

## References

- ADR-1072: `docs/adr/1072-prev-ref-batch-refcount-leak.md` (PR #765)
- ADR-1073: `docs/adr/1073-mcp-score-at-index-eagain-guard.md` (PR #769)
- TSan exclusion list: `.github/workflows/sanitizers.yml` lines 206–210
- req: "Look for new shared state in PRs #765-#771 that lacks TSan coverage. Especially threaded_extract_batch_func paths and sycl_state graph_extractors array"
