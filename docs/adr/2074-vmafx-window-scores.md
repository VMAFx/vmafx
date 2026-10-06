<!-- markdownlint-disable MD013 MD060 -->
# ADR-2074: VMAFx window scores: completion on the feeding thread, one pooling implementation, the window clock and the in-flight bound

- **Status**: Proposed
- **Date**: 2026-10-06
- **Deciders**: RC4 work package 4; maintainer review pending
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
synchronised (design section 2.4), so a library thread cannot call the
engine while the caller does. The engine's synchronous read of a score that
is not final waits for every job in flight (`fence_for_read()`), which would
stall a producer. Scores are written once (ADR-0154). The engine's worker
pool already bounds its queue: a submit waits while `n_threads` jobs wait
(`core/src/thread_pool.c`). And the integer motion extractors derive
`motion2` / `motion3` of every frame at the flush
([ADR-1478](1478-motion-five-frame-window-port.md)), not after the frame that
follows it.

## Decision

We implement window scores with these rules.

1. **Completion is found on the feeding thread.** The calls that change a
   context's scores (`vmafx_submit()`, `vmafx_flush()`,
   `vmafx_context_import_score()`, `vmafx_window_submit()`) look at the open
   windows after they did their work. They are externally synchronised with
   every other engine call, so no engine lock is added to any API function.
   Each window keeps a cursor: the frames before it are known final, a call
   looks only at frames it has not looked at, and it never waits for work in
   flight (no fence). A model's frame is predicted when its inputs are
   written, as the first synchronous read would predict it, so the stored
   score is the one later reads return.
2. **One pooling implementation.** `vmafx_pool_engine()` (`score.c`) is the
   engine's pooling for a target; `vmafx_score_pooled()`,
   `vmafx_feature_score_pooled()`, `vmafx_score_pooled_model_set()` and the
   windows call it with the same arguments, so their values are equal bit for
   bit (HISS-19).
3. **A result is published once.** It is written before a host fence
   ([ADR-1929](1929-vmafx-device-frames-fences.md)) is signalled and never
   after, so `vmafx_window_poll()`, `vmafx_window_wait()` (the fence's timed
   wait, the library's one timed wait) and `vmafx_window_release()` run on any
   thread without the context. A window's own failure is `result.status`; the
   return value of a poll or a wait is about the call.
4. **Callbacks run on one window thread per context**, started by the first
   window with a callback, in completion order, with the context's log sink
   installed. `vmafx_window_release()` cancels an open window (its callback
   never runs), waits for a callback of the window running on the window
   thread unless it is called from that callback (a thread-local marker:
   `_Thread_local`, `__declspec(thread)` under MSVC's C compiler, which
   recognises but does not implement `_Thread_local`), and never frees
   anything under the set's lock. A context holds at most 1024 open windows;
   one more is `VMAFX_E_BUSY`.
5. **The end of a stream.** The stream's last frame is the highest index
   submitted or imported. `vmafx_flush()` completes every open window over
   the frames the stream had, flagged `VMAFX_WINDOW_PARTIAL` when it ended
   before `last`, `VMAFX_E_RANGE` when no scored frame of the window was in
   it. `vmafx_context_destroy()`, once the engine closed (the ADR-1336 retry
   contract is kept), completes the rest with `VMAFX_E_INVALID`, runs every
   queued callback and joins the window thread; window handles outlive their
   context.
6. **The in-flight bound is the engine's queue.** VMAFx adds no second queue.
   `vmafx_context_max_in_flight()` reports the most frames of each input a
   context holds when a submit returns: `R + 2 * T * (R + 1)` with `R` the
   frame retention and `T` the worker threads, plus 1 on a device backend
   (2 * T jobs in flight in any order, each holding its frame and its `R`
   earlier reference frames). The harness measured 6 frames held with T = 2,
   so the former statement "up to `n_threads` further frames" of
   `vmafx_context_frame_retention()` was wrong and is corrected.
7. **The `n_stats` semantics live in the library.** `VmafxWindowClock` cuts a
   stream into windows of `n_stats` seconds (rounded to whole nanoseconds,
   membership in integer arithmetic from the first frame's time) or
   `n_stats_frames` frames (from the first frame's index); a window ends with
   the first frame past it, empty windows are skipped, and the window open at
   the end of the stream is partial unless it is a full window by frame
   count. The FFmpeg filter, the GStreamer element and a live plugin use it
   instead of each writing the rule.
8. **The latency budget.** On the CPU device a window whose frames are final
   per frame completes within two frame periods at 60 fps (33.3 ms) of the
   start of the submit of its last frame, with 0 and 2 worker threads
   (`core/test/test_vmafx_window_live.c`; measured worst about 17 ms). A
   window over `motion2` / `motion3`, and so over any VMAF model, completes
   at the flush until the motion derivation is made incremental
   (`docs/state.md` T-VMAFX-WINDOW-MOTION-AT-FLUSH-2026-10-06).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Find completion on the feeding thread (chosen) | No lock in any existing API function; deterministic; no engine call outside the caller's calls | A frame finished by a worker after the last call is seen by the next call | Chosen: a live producer calls again within a frame period, and the flush completes everything |
| A library thread evaluates windows, with a context lock taken by every engine call | Completion independent of the producer's calls | Every WP2 / WP3 entry point gains a lock; a submit blocked by backpressure would hold it; a model prediction raced with a caller's prediction writes the collector twice | Not chosen |
| Evaluate through the synchronous reads | No new engine entry | `fence_for_read()` waits for every job in flight: the producer stalls for up to 2 * `n_threads` frames per call | Not chosen |
| A VMAFx submit queue and thread | Non-blocking submit for a single-threaded engine | A second bounded queue beside the engine's (HISS-19); engine failures surface one call late | Not chosen; `n_threads` gives the asynchronous submit |
| Callbacks on the feeding thread | No thread | User code runs inside the producer's `vmafx_submit()`; the design names library threads | Not chosen |
| `n_stats` cut by each consumer | No API | The FFmpeg filter, the GStreamer element and a plugin each write the rule and can disagree | Not chosen |
| Windows by time in floating-point seconds | No rounding of `n_stats` | Membership near a boundary depends on rounding of `pts * timebase` | Not chosen: whole nanoseconds |
| Make `motion2` / `motion3` final after the next frame now | Live windows over VMAF models | An engine change in the motion extractor shared with every motion twin, with threading (each frame's SAD comes from a worker), parity re-runs on every backend and its own ADR | Deferred: open item, state row |

## Consequences

- **Positive**: one implementation of pooling and of the window rule for
  every consumer; a poller, a waiter and a callback can be used together on
  any thread; the bound of frames in flight is documented, queried and
  measured; the harness holds every window equal to an offline session and to
  the CLI's frames.
- **Negative**: completion is discovered in the caller's calls, so a producer
  that stops submitting without flushing leaves windows open; windows over
  VMAF models complete at the flush in this release; `vmafx_window_wait()`
  polls (the fence wait of ADR-1929).
- **Neutral / follow-ups**: incremental `motion2` / `motion3` derivation
  (engine, all motion twins; needed for live VMAF windows); the provenance
  pointer of `VmafxWindowResult` (WP5 appends it); the device lanes run the
  harness on their textures; WP9 maps the `window` option group onto
  `VmafxWindowClockConfig`.

## References

- [ADR-1852](1852-vmafx-api-redesign.md), [Research-2158](../research/2158-vmafx-api-redesign.md) section 2.8, [ADR-1897](1897-vmafx-abi-0x-numbering.md), [ADR-1906](1906-vmafx-core-api-semantics.md), [ADR-1929](1929-vmafx-device-frames-fences.md), [ADR-1478](1478-motion-five-frame-window-port.md), [ADR-1336](1336-cuda-context-owned-resource-teardown.md).
- `req` (RC4 work package 4 brief): "Completion rule: every frame in range final, incl. the frame after `last` for `motion2` / `motion3`, and one frame later on device backends; `vmafx_flush` completes open windows (partial flag when the stream ended early)."
- `req` (same brief): "One pooling implementation shared with the sync `vmafx_score_pooled` (HISS-19); results bit-identical."
- `req` (RC4 work-package index, rows added 2026-10-06 after the maintainer's OBS-ready API popup): "Bounded in-flight queue with backpressure, async window scores within a latency budget; headless OBS-pattern harness (texture import per frame, poll from another thread, 60 fps)".
- Issues: [#2138](https://github.com/VMAFx/vmafx/issues/2138), [#2238](https://github.com/VMAFx/vmafx/issues/2238).
- Tests: `core/test/test_vmafx_window.c`, `test_vmafx_window_clock.c`, `test_vmafx_window_live.c`, `test_vmafx_window_cli.py`.
