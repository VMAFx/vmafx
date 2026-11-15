---
paths:
  - core/src/framesync.c
  - core/src/framesync.h
invariant: Framesync buffer error paths invoke vmaf_framesync_abort to prevent consumer cond_wait hang.
area: engine
---
<!-- markdownlint-disable MD013 -->
# Framesync producer error paths and abort signaling

## framesync producer-error paths must call vmaf_framesync_abort (ADR-1092)

`retrieve_filled_data` waits in `pthread_cond_wait` until matching
BUF_FILLED entry appears.  If producer thread exits without calling
`submit_filled_data` for index consumer is waiting on, consumer
hangs forever.

Any code path that acquires framesync buffer (via
`vmaf_framesync_acquire_new_buf`) and then returns error before calling
`vmaf_framesync_submit_filled_data` for that index **must** call
`vmaf_framesync_abort(fs_ctx)` first.  This sets `aborted` flag and
broadcasts on condvar, causing all blocked `retrieve_filled_data` callers
to return `-ECANCELED`.

`vmaf_framesync_destroy` calls `vmaf_framesync_abort` as safety net, but
relying on that alone delays wake-up until destroy time, which is after
`vmaf_thread_pool_wait` — meaning thread pool wait would still hang.

**Rebase-sensitive**: any branch adding new framesync producer paths (feature
extractors, GPU dispatch loops) must include `vmaf_framesync_abort` call on
all error exits from `extract()` or equivalent before returning.
