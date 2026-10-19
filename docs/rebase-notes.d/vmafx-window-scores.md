## VMAFx window scores and the window clock

`rc4/api-wp4-windows`, [ADR-1852](adr/1852-vmafx-api-redesign.md),
[ADR-2074](adr/2074-vmafx-window-scores.md).

- `core/src/vmafx/` gains `window.c` (windows, the completion thread, the
  callback thread, the hooks, the context's engine lock,
  `vmafx_context_max_in_flight()`) and `window_clock.c`.
  `vmafx_engine_enter()` takes the context's engine lock and
  `vmafx_engine_leave()` now takes the context (`vmafx_engine_leave(context,
  previous)`): a rebase that adds an engine call to a WP2 / WP3 function uses
  the pair and never nests it. `submit.c`, `register.c` and `context.c` call
  the hooks `vmafx_windows_note_index()`, `_note_flush()`, `_init()`,
  `_pause()`, `_resume()` and `_close()`; `VmafxContext` (`internal.h`)
  gains `windows`, created with the context. A rebase that reorders those
  functions keeps each hook after the engine call it follows, and the pause
  before `vmaf_engine_close()`.
- `score.c`: the three pooled functions call `vmafx_pool_engine()`, which the
  windows call too; keep one pooling path. `fence.c`'s timed wait is exported
  as `vmafx_host_fence_wait()` for `vmafx_window_wait()`.
- `core/src/libvmaf.c`: `VmafContext` and `struct ThreadDataBatch` gain a
  frame listener (`vmaf_engine_set_frame_listener()`), called at the end of
  `threaded_extract_batch_func()` after the pictures are released; no frame
  count, submit or flush path moved (WP5's `run_note_frame()` calls are
  untouched). `vmaf_engine_score_at_index()` is
  `engine_score_at_index(fence = true)`; new `vmaf_engine_try_score_at_index()`
  (no fence, inputs checked with `vmaf_predict_inputs_written()`),
  `vmaf_engine_feature_written()`,
  `vmaf_engine_try_score_at_index_model_collection()`,
  `vmaf_engine_thread_count()`, `vmaf_engine_subsample()` and
  `vmaf_engine_max_in_flight()`. `core/src/predict.c` gains
  `vmaf_predict_inputs_written()` (collector reads only). An upstream sync of
  `vmaf_score_at_index()` ports into `engine_score_at_index()`; a change to
  `batch_job_take_pictures()`, to the thread pool's enqueue capacity or to the
  device double buffering recomputes `vmaf_engine_max_in_flight()`.
- The branch carries master's #2206 (`read_predicted_collection_score()`) as
  a cherry-pick, which a rebase onto master drops as already applied.
- No score, golden-data or FFmpeg patch impact: a window's values come from
  the synchronous pooling; `libvmaf.h` behaviour is unchanged.
