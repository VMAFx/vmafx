---
paths:
  - core/src/picture.c
  - core/src/picture_pool.c
  - core/src/libvmaf.c
invariant: Picture pool fetch errors signal availability; vmaf_read_pictures centralizes picture ownership release.
area: engine-and-runtime
---
<!-- markdownlint-disable MD013 MD060 -->
# Picture pool geometry, allocation signaling, and picture ownership

- **`picture_compute_geometry` stride alignment uses `unsigned` + `1u`
  mask** (fork-local, round-5 `-fsanitize=integer` sweep):
  `aligned_y` and `aligned_c` in
  [`src/picture.c`](../src/picture.c) are declared `const unsigned` and
  bitmask uses `DATA_ALIGN - 1u` (not `DATA_ALIGN - 1`) so
  complement stays in unsigned domain, avoids signed→unsigned
  implicit conversion that fires with `-fsanitize=integer`. If
  upstream sync rewrites `picture_compute_geometry`, preserve
  `unsigned` type and `1u` literal. See
  [docs/rebase-notes.md](../../docs/rebase-notes.md)
  §PR-fix-picture-align-unsigned-narrowing.
- **`vmaf_picture_pool_fetch` error paths must always signal `pool->available`
  before unlocking** (fork-local, ADR-0960, round-25 audit.2):
  [`src/picture_pool.c`](../src/picture_pool.c) `return_to_pool` block
  must call `pthread_cond_signal(&pool->available)` every time index is
  pushed back to `pool->free_list`, regardless of whether push is from
  normal `vmaf_picture_unref` or from fetch error path. Omitting
  signal on error path creates deadlock: thread already in
  `pthread_cond_wait` (pool exhausted) will never wake. Invariant
  mirrors ADR-0607 (`feedback_shared_resource_outlive_worker_scope`):
  returning resource to pool must always notify waiters. Any rebase or
  refactor adding new `return_to_pool`-equivalent block must preserve
  signal. Covered by
  `test/test_picture_pool_error_paths.c::test_pool_waiter_woken_on_unref`.

- **`context_extract` prev_ref management**: `vmaf_feature_extractor_context_extract()`
  updates `fex->prev_ref` after successful extract when extractor
  carries `VMAF_FEATURE_EXTRACTOR_PREV_REF`. `vmaf_feature_extractor_context_destroy()`
  releases held reference. Any refactor of these functions must preserve
  this pairing so direct callers (unit tests, pool code) observe same
  prev_ref semantics as `vmaf_read_pictures()`.

## Rebase-sensitive invariants (2026-09-02, c-rework-core)

- **`vmaf_read_pictures` picture ownership is centralised in
  `ReadPicturesFrame` helpers** (`src/libvmaf.c`). Four helpers:
  `read_pictures_frame_translate` does CUDA host/device translation.
  `read_pictures_frame_select_host` hands host copies to DNN / worker
  pool only when `HW_FLAG_HOST` is set — zeroed `ref_host` on
  device-only path must never be dereferenced. `read_pictures_frame_cleanup`
  covers every non-batched exit. `read_pictures_frame_cleanup_after_batch`
  is device-only release after `threaded_read_pictures_batch` already unref'd
  host pictures (PR #838); `threaded_read_pictures_batch` must wait for
  final SYCL upload while its original host-picture refs still protect
  storage, before either enqueue-error or success cleanup can unref them
  (BUG-040). serial cleanup must retain same wait before its host
  unrefs and before CUDA's host-cleanup early return in combined CUDA+SYCL
  build. Only `#ifdef HAVE_CUDA` left
  inside `vmaf_read_pictures` guards `read_pictures_frame_translate` call.
  Helper exists only in CUDA builds (CPU no-op stub would leave
  provably-dead error branch that cppcheck flags). Never re-inline further
  backend blocks into `vmaf_read_pictures`; add branches to matching
  helper.
- **PREV_REF references are released only through `fex_release_prev_ref()`**
  and every CPU-pool skip decision goes through `batch_extractor_skip()` /
  `read_pictures_should_skip()`, which share `fex_subsample_skip()` and
  `fex_ctx_runs_on_caller_thread()`. Two skip predicates must agree on which
  extractors worker pool runs, or extractor is dispatched twice (collector
  double-write) or never.
- **Pool or caller = capability, not backend flag.** Extractor with
  `submit()` + `collect()` runs on caller thread (double-buffer path), flag or
  no flag: `VmafFeatureExtractorContext::caller_thread_dispatch`, set once in
  `vmaf_feature_extractor_context_create()`. Pool workers call `extract()`;
  unflagged twin without one (`adm_hip`, `float_vif_hip`) failed every frame
  with `-EINVAL` under `--threads N`
  (T-ASYNC-EXTRACTOR-THREAD-POOL-EINVAL-2026-10-01). Guard:
  `test/test_async_extractor_thread_pool.c`.
