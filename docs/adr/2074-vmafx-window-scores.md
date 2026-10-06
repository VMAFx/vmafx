<!-- markdownlint-disable MD013 MD060 -->
# ADR-2074: VMAFx window scores: a completion thread per context, one pooling implementation, the window clock and the in-flight bound

- **Status**: Accepted
- **Date**: 2026-10-06
- **Deciders**: maintainer (popup 2026-10-06); RC4 work package 4
- **Tags**: api, abi, rc4, scoring, ffmpeg, threading

## Context

[ADR-1852](1852-vmafx-api-redesign.md) and its design review
([Research-2158](../research/2158-vmafx-api-redesign.md), section 2.8) give
the shape of asynchronous window scores: a request for a model, a model set
or a feature pooled with a set of methods over `[first, last]`, completed
when every frame of the range is final, observed by poll, wait or a callback
on a library thread, with the values of the synchronous pooled call. Two
issues add requirements: the per-window statistics of a live encode
(`n_stats` by time or by frame count, a partial last window,
[#2138](https://github.com/VMAFx/vmafx/issues/2138)) and an OBS-ready API
(window scores within a latency budget, a documented maximum of frames in
flight with backpressure, a headless harness with texture import per frame,
polling from another thread and 60 fps pacing,
[#2238](https://github.com/VMAFx/vmafx/issues/2238)).

The engine fixes several facts the design does not. A context is externally
synchronised (design section 2.4): the engine has no lock of its own, so a
library thread that calls it must be kept apart from the caller. The engine's synchronous read of a score that
is not final waits for every job in flight (`fence_for_read()`), which would
stall a producer. Scores are written once (ADR-0154). The engine's worker
pool already bounds its queue: a submit waits while `n_threads` jobs wait
(`core/src/thread_pool.c`). And the integer motion extractors derive
`motion2` / `motion3` of every frame at the flush
([ADR-1478](1478-motion-five-frame-window-port.md)), not after the frame that
follows it.

## Decision

We implement window scores with these rules.

1. **Each context has one completion thread** (maintainer decision): windows
   complete independently of the thread that feeds the context. The thread
   is started by the context's first window and sleeps on a condition
   variable; a generation counter, raised under the set's lock, wakes it. The
   engine's worker jobs raise it through a frame listener
   (`vmaf_engine_set_frame_listener()`, called at the end of each job in
   `threaded_extract_batch_func()`), and so do `vmafx_submit()`,
   `vmafx_flush()`, `vmafx_context_import_score()` (on the device backends
   the collect of the previous frame) and `vmafx_window_submit()`. Each pass
   looks at the open windows once. A score is written once and then final
   (ADR-0154), so each window keeps a cursor: a pass looks only at frames it
   has not looked at, and it never waits for work in flight (no fence).
2. **The engine sees one caller at a time.** Every API call that enters the
   engine goes through `vmafx_engine_enter()`, which now takes the context's
   engine lock; the completion thread's probes and pooling take it too. A
   model's frame is predicted when its inputs are written, as the first
   synchronous read would predict it, so whichever of the two predicts first
   stores the score the other reads. The lock is taken before the set's lock,
   never the other way, and released around every callback.
3. **One pooling implementation.** `vmafx_pool_engine()` (`score.c`) is the
   engine's pooling for a target; `vmafx_score_pooled()`,
   `vmafx_feature_score_pooled()`, `vmafx_score_pooled_model_set()` and the
   windows call it with the same arguments, so their values are equal bit for
   bit (HISS-19).
4. **A result is published once.** It is written before a host fence
   ([ADR-1929](1929-vmafx-device-frames-fences.md)) is signalled and never
   after, so `vmafx_window_poll()`, `vmafx_window_wait()` (the fence's timed
   wait) and `vmafx_window_release()` run on any thread, the feeding thread
   included. A window's own failure is `result.status`; the return value of a
   poll or a wait is about the call.
5. **Callbacks run on a separate callback thread**, started by the first
   window with a callback, in completion order, with the context's log sink
   installed. It is not merged with the completion thread: a callback that
   blocks would otherwise stop the completion of every other window
   (`test_release_of_a_queued_window` holds a callback open and completes a
   second window meanwhile). `vmafx_window_release()` cancels an open window
   (its callback never runs) and waits for a running callback of the window
   unless it is called from that callback (a thread-local marker:
   `_Thread_local`, `__declspec(thread)` under MSVC's C compiler, which
   recognises but does not implement `_Thread_local`); nothing is freed under
   the set's lock. A context holds at most 1024 open windows; one more is
   `VMAFX_E_BUSY`.
6. **The end of a stream and of a context.** The stream's last frame is the
   highest index submitted or imported. `vmafx_flush()` completes every open
   window over the frames the stream had, flagged `VMAFX_WINDOW_PARTIAL` when
   it ended before `last`, `VMAFX_E_RANGE` when no scored frame of the window
   was in it. `vmafx_context_destroy()` pauses the completion thread first;
   the pause waits for the changes already signalled, so a window whose frames
   were final before the destroy completes with its values. If the engine's
   close fails (the ADR-1336 retry contract) the thread resumes and every open
   window stays open; otherwise the rest complete with `VMAFX_E_INVALID`,
   every queued callback runs, both threads are joined, and window handles
   outlive their context.
7. **The in-flight bound is the engine's queue.** VMAFx adds no second queue.
   `vmafx_context_max_in_flight()` reports the most frames of each input a
   context holds when a submit returns: `R + 2 * T * (R + 1)` with `R` the
   frame retention and `T` the worker threads, plus 1 on a device backend
   (2 * T jobs in flight in any order, each holding its frame and its `R`
   earlier reference frames). The harness measured 6 frames held with T = 2,
   so the former statement "up to `n_threads` further frames" of
   `vmafx_context_frame_retention()` was wrong and is corrected.
8. **The `n_stats` semantics live in the library.** `VmafxWindowClock` cuts a
   stream into windows of `n_stats` seconds (rounded to whole nanoseconds,
   membership in integer arithmetic from the first frame's time) or
   `n_stats_frames` frames (from the first frame's index); a window ends with
   the first frame past it, empty windows are skipped, and the window open at
   the end of the stream is partial unless it is a full window by frame
   count. The FFmpeg filter, the GStreamer element and a live plugin use it
   instead of each writing the rule.
9. **The latency budget.** On the CPU device a window whose frames are final
   per frame completes within two frame periods at 60 fps (33.3 ms) of the
   start of the submit of its last frame, with 0 and 2 worker threads
   (`core/test/test_vmafx_window_live.c`; measured worst about 2.3 ms for
   windows submitted ahead, about 19 ms for windows the clock cuts, which are
   submitted with the frame after them), also while the feeder makes no
   further call. A window over `motion2` / `motion3`, and so over any VMAF
   model, completes at the flush until the motion derivation is made
   incremental, which the maintainer placed in RC4 and a separate lane
   implements (`docs/state.md` T-VMAFX-WINDOW-MOTION-AT-FLUSH-2026-10-06).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| A completion thread per context, woken by a condition variable, with the context's engine lock (chosen) | Windows complete when their last frame is final, whatever the feeder does; no polling; one place finds completion | Every API call that enters the engine takes a lock; a submit blocked by backpressure holds it while the completion thread waits | Chosen by the maintainer ("Dedicated completion thread") |
| Find completion on the feeding thread (this ADR's first proposal) | No lock in any existing API function; deterministic; no engine call outside the caller's calls | A frame a worker finishes after the feeder's last call stays unseen until the feeder calls again: a stalled producer, or one that waits for its windows before it feeds, never sees them complete | Not chosen: windows must complete independently of the feeding thread (maintainer, 2026-10-06); `test_window_completes_while_the_feeder_stalls` fails without the frame listener |
| One thread for completion and callbacks | One thread per context | A callback that blocks stops the completion of every other window | Not chosen: two threads, completion never waits for user code |
| An eventfd-style counter instead of the condition variable | A file descriptor a caller could poll | Not portable to the Windows pthread subset; the counter is a field under the set's lock already | Not chosen |
| Evaluate through the synchronous reads | No new engine entry | `fence_for_read()` waits for every job in flight | Not chosen |
| A VMAFx submit queue and thread | Non-blocking submit for a single-threaded engine | A second bounded queue beside the engine's (HISS-19); engine failures surface one call late | Not chosen; `n_threads` gives the asynchronous submit |
| `n_stats` cut by each consumer | No API | The FFmpeg filter, the GStreamer element and a plugin each write the rule and can disagree | Not chosen |
| Windows by time in floating-point seconds | No rounding of `n_stats` | Membership near a boundary depends on rounding of `pts * timebase` | Not chosen: whole nanoseconds |
| Make `motion2` / `motion3` final after the next frame in this lane | Live windows over VMAF models | An engine change in the motion extractor shared with every motion twin, with its own ADR and parity runs | Not here: the maintainer placed it in RC4 as a separate lane ("RC4, required for #2138/#2238") |

## Consequences

- **Positive**: windows complete independently of the feeder; one
  implementation of pooling and of the window rule for every consumer; a
  poller, a waiter and a callback can be used together on any thread; the
  bound of frames in flight is documented, queried and measured; the harness
  holds every window equal to an offline session and to the CLI's frames.
- **Negative**: two threads per context once windows are used; every engine
  call of the API takes the engine lock (uncontended without windows); windows
  over VMAF models complete at the flush until the motion lane lands;
  `vmafx_window_wait()` polls the fence (ADR-1929).
- **Neutral / follow-ups**: the incremental `motion2` / `motion3` lane; the
  provenance pointer of `VmafxWindowResult` (WP5 appends it); the device lanes
  run the harness on their textures; WP9 maps the `window` option group onto
  `VmafxWindowClockConfig`.

## References

- [ADR-1852](1852-vmafx-api-redesign.md), [Research-2158](../research/2158-vmafx-api-redesign.md) section 2.8, [ADR-1897](1897-vmafx-abi-0x-numbering.md), [ADR-1906](1906-vmafx-core-api-semantics.md), [ADR-1929](1929-vmafx-device-frames-fences.md), [ADR-1478](1478-motion-five-frame-window-port.md), [ADR-1336](1336-cuda-context-owned-resource-teardown.md).
- `Q` (maintainer popup 2026-10-06, how windows complete): "Dedicated completion thread".
- `Q` (maintainer popup 2026-10-06, VMAF windows complete at the flush): "RC4, required for #2138/#2238 (Recommended)".
- `req` (RC4 work package 4 brief): "Completion rule: every frame in range final, incl. the frame after `last` for `motion2` / `motion3`, and one frame later on device backends; `vmafx_flush` completes open windows (partial flag when the stream ended early)."
- `req` (same brief): "One pooling implementation shared with the sync `vmafx_score_pooled` (HISS-19); results bit-identical."
- `req` (RC4 work-package index, rows added 2026-10-06 after the maintainer's OBS-ready API popup): "Bounded in-flight queue with backpressure, async window scores within a latency budget; headless OBS-pattern harness (texture import per frame, poll from another thread, 60 fps)".
- Issues: [#2138](https://github.com/VMAFx/vmafx/issues/2138), [#2238](https://github.com/VMAFx/vmafx/issues/2238).
- Tests: `core/test/test_vmafx_window.c`, `test_vmafx_window_clock.c`, `test_vmafx_window_live.c`, `test_vmafx_window_cli.py`, `test_vmafx_lifetime.c` (`test_failed_destroy_keeps_windows`).
