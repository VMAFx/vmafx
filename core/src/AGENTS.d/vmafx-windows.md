---
paths:
  - core/src/vmafx/window.c
  - core/src/vmafx/window_clock.c
  - core/src/vmafx/score.c
  - core/src/predict.c
  - core/test/test_vmafx_window*
  - core/test/vmafx_window_test_util.h
invariant: Window values = sync pooled call bit for bit (one vmafx_pool_engine); completion on feeding thread, never a fence.
---
<!-- markdownlint-disable MD013 -->
# VMAFx window scores (ADR-2074)

- Files: `window.c` (windows, set, window thread, hooks, `vmafx_context_max_in_flight`), `window_clock.c` (`VmafxWindowClock`, #2138 `n_stats` rule), `score.c` (`vmafx_pool_engine()`, `vmafx_pool_target_name()`: the ONE pooling of sync calls + windows, HISS-19). Never a second pooling path.
- Completion found only on feeding thread: hooks `vmafx_windows_note_index()` (end of `vmafx_submit()` + `vmafx_context_import_score()`), `vmafx_windows_note_flush()` (end of `vmafx_flush()`), `vmafx_windows_close()` (`vmafx_context_destroy()` after successful engine close, before `release_held()`), plus `vmafx_window_submit()`. No engine call from any other thread; no lock added to WP2 / WP3 entry points.
- Probes never fence: `vmaf_engine_feature_written()`, `vmaf_engine_try_score_at_index()` (= `engine_score_at_index(fence=false)`, needs `vmaf_predict_inputs_written()` first), `vmaf_engine_try_score_at_index_model_collection()`. `fence_for_read()` waits whole thread pool -> producer stall; never call sync score fns from window code. -EAGAIN / -EINVAL = not final before flush, `VMAFX_E_NOTFOUND` after.
- Cursor per window: `[first, cursor)` final (scores write-once, ADR-0154). Skip `i % n_subsample` frames; `n_scored` = multiples in range. Stream end = `context->scored_last` (max submitted or imported index), `have_scored`.
- Model probe writes model score (same predict as first sync read). Model-set probe needs #2206 (`read_predicted_collection_score()`): set scoring idempotent.
- Publish: result written once, then `vmafx_host_fence_signal(done)`; poll / wait read after signal, lock-free. `vmafx_window_wait()` = `vmafx_host_fence_wait()` (fence.c, one timed wait; shim has no `pthread_cond_timedwait`).
- Locks: `VmafxWindowSet.lock` guards open list, queue, `released`, `queued`, `open`, `delivering`. Never free under lock: window's last ref drops set ref; set freed by last window after context gone. Evaluate on a ref'd snapshot (<= 1024, stack array).
- Window thread: lazily by first window with callback; context sink copy installed; TLS `vmafx_delivering_set` (`_Thread_local`, `__declspec(thread)` for MSVC C: recognises not implements `_Thread_local`). Release waits for running callback unless TLS says self. Bounded loop `VMAFX_WINDOW_MAX_DELIVERIES`.
- Open windows capped 1024 -> `VMAFX_E_BUSY` naming `context` (documented).
- In-flight bound = engine `thread_pool.c` queue (no VMAFx queue): `vmaf_engine_max_in_flight()` = `R + 2*T*(R+1)` + 1 device. Change of `batch_job_take_pictures()`, enqueue capacity or GPU double buffer -> recompute + `test_vmafx_window_live` unpaced case. Former doc "up to n_threads" wrong (6 measured, T=2).
- motion2 / motion3 final only at flush (`motion_window.h`, ADR-1478): windows over them / any VMAF model complete at flush. Do not paper over in window code; fix = incremental derivation in engine (state T-VMAFX-WINDOW-MOTION-AT-FLUSH-2026-10-06).
- Clock: time windows in integer ns (`nearbyint(n_stats * 1e9)`, >= 1, <= 9.2e9 s), from first frame's pts; frame windows from first index; close on first frame past window; skip empty; finish partial unless full frame window; refused frame leaves clock unchanged.
- Tests: `test_vmafx_window` (public; sync == async every pool, flush partial, subsample, motion, model + set, 1024 open, callbacks, destroy, refusals), `test_vmafx_window_clock`, `test_vmafx_window_live` (Linux, static lib: 60 fps texture import, poller thread, latency <= 2 periods unless sanitizer build, backpressure, host copies 0, release races), `test_vmafx_window_cli.py` (harness windows == CLI per-frame scores pooled with engine arithmetic). Live tests `is_parallel: false` (timed). TSan: `-Db_sanitize=thread` build, run window tests.
