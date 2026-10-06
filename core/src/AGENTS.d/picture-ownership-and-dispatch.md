---
paths:
  - core/src/libvmaf.c
  - core/src/feature/feature_extractor.cpp
invariant: PREV_REF counted refs, n-2 only for a reader; read_pictures owns both pictures; advance after each frame.
---
<!-- markdownlint-disable MD013 -->
# Picture ownership, batch dispatch, and SYCL upload synchronization

## Extractor advance after each frame and read fence (ADR-2090)

`advance_extractors()` calls `fex->advance()` (optional; motion window
extractors) on every registered context at the end of a successful
`vmaf_engine_read_pictures()` (via `read_pictures_frame()`),
`vmaf_read_pictures_sycl()`, `fence_for_read()` and `vmaf_engine_advance()`
(VMAFx completion thread, engine lock held); never after flush, never
concurrent with that context's extract / collect / flush, never from a worker. Pooled CPU
extractor (worker pool): advance runs on the registered context (never
extracts), engine sets `is_initialized` so close frees what advance built, as
the threaded flush does. Other contexts: only once initialised. Keep the call
on every frame-feeding entry point; a new entry point without it makes
motion2 / motion3 final only at flush there. `vmaf_engine_feature_score_at_index()`
fences on `-EINVAL` too when `index <= last_index` (fed frame without a slot
yet), then re-reads; unknown name still `-EINVAL`.
Guards: `test_motion_window_incremental` (threaded runs fail without the
fence), `test_vmafx_window`, `test_motion_window_advance_contract.py`.

## PREV_REF window: earlier reference frames, counted references (ADR-1072, ADR-0778, ADR-1478)

Context keeps reference picture of frame n-1 (`vmaf->prev_ref`). Frame n-2
(`vmaf->prev_prev_ref`; Netflix `a2b59b77`) only while
`vmaf->keep_prev_prev_ref`: set by `admit_prev_prev_ref()` when a registered
extractor's `reads_prev_prev_ref()` answers true (`motion` / `motion_v2` with
`motion_five_frame_window=true`). Netflix keeps n-2 always; fork keeps it only
for a reader, so without one a context holds exactly what it held before the
port (deliberate deviation, ADR-1478; no score moves). With keep:
`read_pictures_update_prev_ref()` unrefs n-2, moves n-1 down with its count,
refs current frame; without: unref n-1, ref current frame.
`vmaf_commit_remaining_owners()` releases both.

Every PREV_REF dispatch path (`batch_extract_one()` on a worker,
`read_pictures_dispatch_one()` serial, `read_pictures_cuda_submit_current()`)
hands the extractor its **own counted references** through
`fex_take_prev_refs()` (n-2 only to a reader) and releases them through
`fex_release_prev_ref()`.
Between the two, `vmaf_feature_extractor_context_extract()`:

- **SUCCESS**: PREV_REF swap in `feature_extractor.cpp`: for a reader unrefs
  `fex->prev_prev_ref`, moves `fex->prev_ref` into it (count kept); for any
  other extractor unrefs `fex->prev_ref`; then refs the current frame into
  `fex->prev_ref`.
- **ERROR**: both fields unchanged.

Either way each field holds at most one count afterwards and
`fex_release_prev_ref()` drops both. A bare `memset` or a struct copy in place
of take / release leaks one picture-pool slot per frame or frees a picture an
extractor still reads; pool then deadlocks in `vmaf_picture_pool_fetch()`.
Worker batch: snapshots `f->prev_ref` / `f->prev_prev_ref` taken before the
context's window advances, released once by `threaded_extract_batch_func()`
after every extractor took and balanced its own counts.

Pool rule (ADR-1478), fail closed, never a stall: with keep, a preallocated
pool needs `pic_cnt >= VMAF_PICTURE_POOL_MIN_PREV_PREV_REF` (4); whichever of
`vmaf_preallocate_pictures()` / registration (`vmaf_use_feature()`,
`vmaf_use_features_from_model()`, ADR-1324 context fallback, test append
helper) comes second returns -EINVAL with one log line naming `pic_cnt` and
the minimum. Without keep: no minimum, as before. Default pool
`n_threads * 2`, `+ 2` with keep (`check_picture_pool()`). CLI
`thread_cnt > 0 ? (thread_cnt + 1) * 2 + 1 : 4` plus read-ahead
(`core/tools/vmaf.cpp`): sized before models load, so serial takes 4. A pool
of 3 with keep would hang on frame 2. Guards: `test_motion_five_frame_window`
(`test_pool_of_four_pictures`, `test_pool_of_three_without_the_window`,
`test_pool_below_four_is_refused`), `test_read_pictures_failure_ownership`,
`test_thread_safety_batch`.

**Rebase-sensitive**: upstream `libvmaf.c` struct-copies both pictures into
the extractor and zeroes them after `extract()`. Keep fork's counted
references; take upstream's window semantics only.

## `vmaf_read_pictures()` owns both pictures on every return (ADR-1431)

- Context + two pictures given -> every return releases both once. No early
  `return err;` between argument checks and extractor loop: pool slot leaks,
  `vmaf_picture_pool_close()` waits forever in `vmaf_close()`, `vmaf` CLI hangs
  holding the device lock after a VRAM out-of-memory (Netflix/vmaf#1420 on fork).
- Validation + prep failures -> `read_pictures_frame_cleanup()`. CUDA translation
  failure -> `read_pictures_translate_abort()`: translations sharing `priv` with
  caller's picture released with it, fresh ones (ring pictures, downloaded host
  copy) released here, fresh device stream drained first (upload reads the host
  picture).
- No context / one picture `NULL` / flush call (both `NULL`) -> takes nothing.
- Guards: `test_read_pictures_failure_ownership` (CPU, pool with no spare
  picture, alarm turns hang into failure), `test_cuda_oom_pictures_released`.

## SYCL shared uploads finish before either picture cleanup returns (BUG-040)

`vmaf_sycl_shared_frame_upload()` reads caller's host-backed reference and
distorted pictures asynchronously on in-order `copy_queue`. Its saved
`last_upload_event` is final distorted-plane copy, so waiting on that one
event also orders every earlier reference and distorted copy without draining
independent compute queue.

Both ownership exits in `libvmaf.c` must preserve that wait:

- `read_pictures_frame_cleanup()` waits before its direct picture unrefs.
- `threaded_read_pictures_batch()` waits after enqueue while caller's
  original counted references are still live, then unrefs those references.
  worker may finish and drop its own copies before wait, so moving
  barrier to `read_pictures_frame_cleanup_after_batch()` is too late:
  release callback can already have poisoned or recycled host storage.

Do not replace either event wait with global queue/device wait, and do not
remove threaded wait because one timing sample happened to let DMA finish
before worker. In combined CUDA+SYCL build, wait must remain before
CUDA's host-cleanup early return. `core/test/test_sycl_cuda_serial_upload_lifetime.c`
pins that compile combination through public API with `n_threads=0` and
`n_threads=1`; 4K release callback poisons host storage as soon as its final
reference drops and PSNR proves DMA already consumed original pixels.
`testdata/test_sycl_4k_repeat_determinism.py` then covers 20 serial and 20
`--threads 1` runs against full normalized score report.
