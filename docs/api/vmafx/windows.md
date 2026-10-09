# VMAFx window scores

A window score is a pooled score over a range of frames that you ask for
before the frames are final and read when they are: the per-window
statistics of a live encode (`n_stats`,
[#2138](https://github.com/VMAFx/vmafx/issues/2138)), or a rolling score a
live-production plugin shows without blocking its render thread
([#2238](https://github.com/VMAFx/vmafx/issues/2238)). The calls are part of
the [VMAFx API preview](index.md) and are declared in `vmafx/score.h`; the
[reference page](score.md) lists every field. The design is recorded in
[ADR-2074](../../adr/2074-vmafx-window-scores.md).

## Ask for a window

```c
VmafxWindowRequest req = VMAFX_WINDOW_REQUEST_INIT;
req.target = VMAFX_WINDOW_TARGET_MODEL;   /* or MODEL_SET, FEATURE */
req.model = model;                        /* the window holds a reference */
req.pool_mask = VMAFX_POOL_MASK_MIN | VMAFX_POOL_MASK_MAX |
                VMAFX_POOL_MASK_MEAN | VMAFX_POOL_MASK_HARMONIC_MEAN;
req.first = 48;                           /* inclusive, as vmafx_score_pooled() */
req.last = 95;

VmafxWindow *window = NULL;
status = vmafx_window_submit(context, &req, &window, &error);
```

`vmafx_window_submit()` returns at once. Submit windows before their frames,
while they arrive, or after the flush; a window whose frames are all final
completes right after the submit. At most 1024 windows of one context are
open at a time; one more is refused with `VMAFX_E_BUSY` naming `context`.

| Target | Set | Same values as |
| --- | --- | --- |
| `VMAFX_WINDOW_TARGET_MODEL` | `model` | `vmafx_score_pooled()` |
| `VMAFX_WINDOW_TARGET_MODEL_SET` | `model_set` | `vmafx_score_pooled_model_set()` |
| `VMAFX_WINDOW_TARGET_FEATURE` | `feature` (copied) | `vmafx_feature_score_pooled()` |

`pool_mask` has bit `1 << p` for each `VmafxPool` method `p`
(`VMAFX_POOL_MASK_MIN` to `VMAFX_POOL_MASK_PERC20`); one window computes all
of them.

## When a window completes

A window completes when every scored frame of `[first, last]` has its final
score, and then it is never revised. Each context has a completion thread,
started by its first window, that finds this out: the worker thread that
finishes a frame wakes it, and so do `vmafx_submit()`, `vmafx_flush()`,
`vmafx_context_import_score()` and `vmafx_window_submit()`. It sleeps until
woken (no polling), looks only at frames it has not looked at before, and
never waits for work still on a worker thread. A window therefore completes
when its last frame is final, whether or not the thread that feeds the
context calls again.

- **Without worker threads** (`n_threads` 0) a window over frames that are
  final per frame completes right after the submit of its last frame.
- **With worker threads** it completes when the worker finishes its last
  frame, also while the producer is busy elsewhere or waiting: about 2 ms
  after the start of that frame's submit on the Netflix 576x324 pair.
- **On a device backend** a frame's scores are collected when the next frame
  is submitted (the engine double-buffers device extractors), one submit
  later.
- **Features that read the next frame.** `motion2` and `motion3` of a frame
  need the motion of the frame after it (and, with
  `motion_five_frame_window`, frames 0 and 1 need frame 2). The engine
  derives them as soon as that frame is scored
  ([ADR-2090](../../adr/2090-motion-window-incremental.md)), so a window over
  them, or over a VMAF model (every model reads `motion2`), completes one
  frame after its `last`: without worker threads right after the submit of
  frame `last + 1`; with worker threads when the worker finishes frame
  `last + 1` (the completion thread lets the engine derive them before it
  looks), also while the producer makes no further call; on a device backend
  one submit later, and with `motion_cuda` at its readback batch of eight
  frames. The last frame of the stream has no frame after it: its `motion2`
  and `motion3` become final at the flush, which completes the windows that
  end there.
- **At the flush** every open window completes over the frames the stream
  had. A window whose `last` lies past the stream's last frame is flagged
  `VMAFX_WINDOW_PARTIAL` and pools `n_frames` frames from `first`; one that no
  frame of the stream reaches completes with `VMAFX_E_RANGE`.
- **When the context is destroyed** every open window completes with
  `VMAFX_E_INVALID`, and every callback runs before `vmafx_context_destroy()`
  returns. Window handles stay valid until you release them.

The stream's last frame is the highest index submitted or imported with
`vmafx_context_import_score()`, so a window over scores you import completes
when its last score is imported, with or without a flush.

## Read the result

Three ways, all thread-safe, and you may use them together:

| Call | Returns |
| --- | --- |
| `vmafx_window_poll(window, &result, &error)` | `VMAFX_OK` and the result once complete, else `VMAFX_PENDING` without an error |
| `vmafx_window_wait(window, timeout_ns, &result, &error)` | The same after waiting up to `timeout_ns` (`UINT64_MAX`: no limit) |
| `req.on_complete(window, &result, req.user)` | Called once on the context's callback thread |

```c
VmafxWindowResult r = VMAFX_WINDOW_RESULT_INIT;
if (vmafx_window_poll(window, &r, NULL) == VMAFX_OK && r.status == VMAFX_OK)
    printf("frames %llu-%llu: mean %.6f, min %.6f%s\n",
           (unsigned long long)r.first, (unsigned long long)(r.first + r.n_frames - 1),
           r.value[VMAFX_POOL_MEAN], r.value[VMAFX_POOL_MIN],
           (r.flags & VMAFX_WINDOW_PARTIAL) ? " (partial)" : "");
vmafx_window_release(window);
```

| Field | Meaning |
| --- | --- |
| `status` | `VMAFX_OK`, or why the window has no values (`VMAFX_E_RANGE`: no scored frame of it in the stream; `VMAFX_E_NOTFOUND`: a frame has no score of the target after the flush; `VMAFX_E_INVALID`: the context was destroyed first). The failure is also logged to the context |
| `flags` | `VMAFX_WINDOW_PARTIAL` when the stream ended before `last` |
| `first`, `last` | As requested |
| `n_frames` | Frames pooled over, from `first` |
| `n_scored` | Those the context scored: with `n_subsample` > 1 only every `n_subsample`-th index |
| `value[p]` | The pooled score of method `p` for each bit of `pool_mask` (a model set's bagging score) |
| `stddev[p]`, `ci95_lo[p]`, `ci95_hi[p]` | A model set's other three bootstrap values |
| `name` | The model's, the set's or the feature's name |

Every value is the synchronous call's over the same frames, bit for bit: a
window and `vmafx_score_pooled()` run one pooling implementation with the same
arguments. A failure of the window itself is in `result.status`; the return
value of a poll or a wait is about the call.

## Threads

- The calls that feed a context (`vmafx_submit()`, `vmafx_flush()`,
  `vmafx_context_import_score()`, `vmafx_window_submit()`) are externally
  synchronised, as every context call is. The completion thread uses the
  context's engine between them: each call that enters the engine takes the
  context's engine lock, so the engine sees one caller at a time.
- `vmafx_window_poll()`, `vmafx_window_wait()` and `vmafx_window_release()`
  may run on any thread at any time, the feeding thread included: waiting
  there for a window whose frames are on the workers returns when they finish.
- Callbacks run on one callback thread per context, started by the first
  window that has a callback, in completion order. It is not the completion
  thread, so a slow callback delays later callbacks but never the completion
  of a window. A callback may poll, wait on and release windows, its own
  included, and must not call anything else on the window's context: no
  `vmafx_submit()`, no `vmafx_flush()`, no `vmafx_context_destroy()`. A
  message the library raises during a callback reaches the context's log
  callback.
- `vmafx_window_release()` cancels an open window: its callback never runs.
  Released from another thread while its callback runs, the release waits for
  the callback to return; released from that callback, it does not wait.
- `vmafx_context_destroy()` first lets the completion thread finish the changes
  already signalled, so a window whose frames were final before the destroy
  completes with its values; then it completes the rest with
  `VMAFX_E_INVALID`, runs every callback and stops both threads. A destroy
  that fails (ADR-1336) leaves the threads running and every open window open.

## Cut a stream into windows: the window clock

`n_stats` and `n_stats_frames` (the FFmpeg filter's `window` option group)
mean the same in every consumer because the library cuts the windows:

```c
VmafxWindowClockConfig cfg = VMAFX_WINDOW_CLOCK_CONFIG_INIT;
cfg.n_stats = 2.0;                 /* seconds; or cfg.n_stats_frames = 48 */
VmafxWindowClock *clock = NULL;
vmafx_window_clock_create(&cfg, &clock, &error);

/* per frame, before or after submitting it: */
VmafxWindowSpan span = VMAFX_WINDOW_SPAN_INIT;
if (vmafx_window_clock_frame(clock, index, pts_ns, &span, NULL) == VMAFX_OK)
    submit_window(span.first, span.last);       /* the window that just ended */

/* at the end of the stream: */
if (vmafx_window_clock_finish(clock, &span, NULL) == VMAFX_OK)
    submit_window(span.first, span.last);       /* flagged VMAFX_WINDOW_PARTIAL */
vmafx_window_clock_destroy(clock);
```

- **By time**: window `k` holds the frames whose presentation time lies in
  `[t0 + k * n_stats, t0 + (k + 1) * n_stats)`, `t0` the first frame's.
  `n_stats` is rounded to whole nanoseconds, so membership is exact integer
  arithmetic. `span.start_ns` and `span.end_ns` are the window's bounds.
- **By frame count**: window `k` holds the frames whose index lies in
  `[i0 + k * n_stats_frames, i0 + (k + 1) * n_stats_frames)`. `span.start_ns`
  and `span.end_ns` are its first and last frame's times.
- A window ends when the first frame past it arrives, so its span comes with
  that frame. Windows without a frame (a gap in the stream) are skipped:
  `span.window` can jump.
- `vmafx_window_clock_finish()` returns the window open at the end of the
  stream, flagged `VMAFX_WINDOW_PARTIAL` unless it is a full window by frame
  count, or `VMAFX_PENDING` when none is open.
- Indices must increase and times must not decrease; a refused frame leaves
  the clock as it was.

Submitting the span right away, before the frame that ended it is submitted,
lets the window complete in that submit when its frames are final.

## Frames in flight and backpressure

`vmafx_context_max_in_flight(context)` is the most frames of each input the
context holds when `vmafx_submit()` returns, whatever your rate. With
`R = vmafx_context_frame_retention()` and `T` worker threads it is
`R + 2 * T * (R + 1)`, plus 1 on a device backend:

- without worker threads a submit scores its frame before it returns, and the
  context keeps the `R` reference frames temporal extractors read;
- with them, a submit returns once its frame is queued and waits while `T`
  frames wait for a worker, so at most `2 * T` frames are in flight, each with
  its `R` earlier reference frames. A faster producer is held back in
  `vmafx_submit()`; the queue never grows past the bound.

A producer that recycles its frames (a ring of textures, a
`VmafxFramePool`) needs `vmafx_context_max_in_flight()` frames plus the one it
is filling, and reuses a frame once its release fence
(`vmafx_frame_release_fence()`) is signalled.

## A live plugin

`core/test/test_vmafx_window_live.c` imitates a live-production plugin on the
CPU device: a render thread paces frames at 60 per second, renders each into
a texture of a ring of `vmafx_context_max_in_flight() + 1`, signals its
acquire fence, imports it with `vmafx_context_import_frame()` and submits it;
a window clock cuts 0.2-second windows, and windows of 12 frames are also
submitted ahead; a poller thread polls every window. It holds that

- every window equals an offline session on the same frames bit for bit, and
  `core/test/test_vmafx_window_cli.py` holds the windows against the `vmaf`
  CLI's per-frame scores of the same pair, pooled with the engine's
  arithmetic;
- a window over frames that are final per frame completes within two frame
  periods (33.3 ms) of the start of the submit of its last frame, with 0 and
  2 worker threads: measured worst about 2.3 ms for windows submitted ahead
  and about 19 ms for windows the clock cuts (those are submitted one frame
  later, with the frame that ends them);
- a window completes while the producer stalls: with 2 worker threads every
  window's frame was still on a worker when its submit returned, and the
  window completed about 2 ms later with no further call;
- the textures the library holds never exceed the bound, also when the
  producer is not paced;
- no frame is copied through the host (the host-copy counter stays 0).

Run it with the fixtures in `python/test/resource/yuv`:

```bash
python3 scripts/ci/run_meson_test.py -- -C build test_vmafx_window \
    test_vmafx_window_clock test_vmafx_window_live test_vmafx_window_cli
```

The device lanes run the same harness on CUDA, SYCL, HIP and Metal textures.
