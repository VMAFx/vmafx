<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-2090: Derive `motion2` / `motion3` frame by frame: final once the frame after is scored, the same statements as the flush

- **Status**: Accepted (2026-10-07, Q-088)
- **Date**: 2026-10-06
- **Deciders**: maintainer (popup 2026-10-07, Q-088); lusoris (decision Q-038); RC4 motion lane
- **Tags**: `motion`, `feature-extractor`, `engine`, `threading`, `gpu-parity`, `api`, `rc4`

## Context

`motion2` of frame `i` is the smaller of the SAD scores on both sides of it,
and `motion3` blends `motion2` and, with `motion_moving_average`, averages it
with the frame before. Since [ADR-1478](1478-motion-five-frame-window-port.md)
the integer motion extractors (`motion`, `motion_v2`) store one SAD score per
frame and derive `motion2` / `motion3` of every frame in `flush()`, through
one function, `vmaf_motion_window_flush()`
(`core/src/feature/motion_window.h`), which the GPU twins call too. That is
upstream's design (Netflix `a4a1492d`), and it makes every score of those two
features, and so every VMAF model score (each built-in model reads
`motion2`), final only at the end of the stream.

The VMAFx window scores of [ADR-2074](2074-vmafx-window-scores.md) found the
consequence (`docs/state.md` `T-VMAFX-WINDOW-MOTION-AT-FLUSH-2026-10-06`): a
window over a VMAF model completes at the flush, so the live per-window
statistics of [#2138](https://github.com/VMAFx/vmafx/issues/2138) and a
rolling score in a live plugin
([#2238](https://github.com/VMAFx/vmafx/issues/2238)) arrive after the
stream ends. The maintainer put live VMAF windows in the 1.0 scope (Q-038):
`motion2` / `motion3` have to be computed as the frames come in, with every
value bit-identical to the flush-time computation.

Three constraints shape the change. With worker threads each worker runs a
private copy of a CPU extractor and appends the SAD scores of an arbitrary
subset of frames in any order (ADR-1478, alternative 2), so no extractor
instance sees the stream in order. A device twin appends a frame's SAD when
the engine collects it, one `vmaf_read_pictures()` later (`motion_cuda`: at
the end of its readback batch of eight frames, ADR-0845). And the engine's
synchronous read fences only when the collector reports an existing, unwritten
slot (`-EAGAIN`, Netflix#1305); a fed frame whose feature has no slot yet, or
whose index lies past the vector's capacity of eight, answered `-EINVAL` at
once, so a read issued while a worker still holds the frame said "invalid"
instead of waiting.

## Decision

1. **The rule.** Frame `i`'s `motion2` and `motion3` are *final* (appended to
   the collector, write-once) as soon as the SAD scores of frames `0` to
   `max(i + 1, min_idx)` are in the collector, where `min_idx` is 1, or 2 with
   `motion_five_frame_window`. That is the earliest point at which the flush
   over the whole stream would compute the same values: `motion2` has the SAD
   of the frame after it, the frames below `min_idx` have their stamp (the
   SAD of frame `min_idx`), and `motion3` has the moving-average state of
   frames `0` to `i - 1`. The last frame has no frame after it; its scores,
   and those of every frame of a stream with `min_idx` frames or fewer, are
   written by the flush.
2. **One derivation, the same statements.** `vmaf_motion_window_advance()`
   derives every complete frame and `vmaf_motion_window_flush()` the rest,
   both in `integer_motion.c`. Both run upstream's per-frame statements
   (`motion_flush_one()`, unchanged) in index order and carry the two values
   upstream's loop carries, the stamp and the previous processed value, in a
   `VmafMotionWindowState` held by the extractor (`n_sad`, `next`,
   `stamp_value`, `prev_processed`). The stamp is computed once, by whichever
   call derives frame 0, with the statement of upstream's flush. A flush
   without a state derives every frame in one call, as before. A retried
   flush and an advance after the flush append nothing.
3. **The engine drives it, serialised with the feeding thread.**
   `VmafFeatureExtractor` gains an optional `advance()` callback. The engine
   calls it on every registered extractor that has one
   (`advance_extractors()`, `core/src/libvmaf.c`) at the end of every frame
   it accepts (`vmaf_engine_read_pictures()`, `vmaf_read_pictures_sycl()`),
   at the end of a read fence (`fence_for_read()`), and in
   `vmaf_engine_advance()`, which the VMAFx completion thread of
   ADR-2074 calls under the context's engine lock before each pass
   (`advance_engine()` in `core/src/vmafx/window.c`). Every call is
   serialised with the context's other engine calls (the feeding thread, or
   the engine lock), never concurrent with that instance's `extract()`,
   `collect()` or `flush()`, and never after the flush. With worker threads
   it advances the registered context of a pooled extractor, which never
   extracts: the advance builds its feature-name dictionary on first use and
   the engine marks the context initialised, as the threaded flush does, so
   `close()` frees it.
4. **The frame-final signal is the collector write.** A frame is final when
   its `motion2` / `motion3` entry is written; there is no separate event.
   The VMAFx completion thread of ADR-2074 observes it with its fence-free
   probes: the advance runs before `vmaf_engine_read_pictures()` returns,
   that is before `vmafx_submit()` wakes the thread, and the thread itself
   advances the engine before it probes after a worker's frame listener woke
   it. When it happens:
   - without worker threads, in `vmaf_read_pictures()` of frame `i + 1`
     (frame 2 for frames 0 and 1 of the five-frame window);
   - with worker threads, when the worker finishes that frame: its frame
     listener wakes the completion thread, which advances the engine (a
     libvmaf caller without VMAFx windows sees it at its next
     `vmaf_read_pictures()` or score read);
   - on a device backend, when the twin's SAD of that frame is collected:
     one `vmaf_read_pictures()` later; `motion_cuda` at the end of its
     readback batch (up to eight frames later, ADR-0845, unchanged here).
5. **A synchronous read of a fed frame waits before it says "not yet".**
   `vmaf_engine_feature_score_at_index()` fences on `-EINVAL` as well as
   `-EAGAIN` when the index is at most the last frame fed, so a read after
   `vmaf_read_pictures(i + 1)` returns frame `i`'s `motion2` with worker
   threads too. A name no extractor writes still answers `-EINVAL`, after
   the fence.
6. **Every twin that derived at the flush derives frame by frame.** The CPU
   extractors `motion` and `motion_v2`; `motion_v2_cuda`, `motion_v2_sycl`,
   `motion_v2_hip`, `motion_v2_metal` (both windows);
   `integer_motion_metal` (both windows); `motion_cuda`, `motion_sycl`,
   `motion_hip` (the five-frame window). Each holds a
   `VmafMotionWindowState`, builds its window once for `advance()` and
   `flush()`, and registers `.advance`. The three-frame paths of
   `motion_cuda`, `motion_sycl` and `motion_hip`, and `float_motion` on every
   backend, already wrote frame `i - 1` while collecting frame `i`; they are
   unchanged. `motion_force_zero` on the twins that write every score in
   `collect()` clears or skips the advance, as it clears the flush.
7. **This amends [ADR-2074](2074-vmafx-window-scores.md)** (Accepted), the
   sentence of decision 9 and of its Negative consequences that windows over
   `motion2` / `motion3` and over VMAF models complete at the flush, and its
   alternative "Make `motion2` / `motion3` final after the next frame in this
   lane": they complete one frame after their `last` (rule 4).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Engine `advance()` hook, serialised with the feeding thread (feeding calls, read fence, VMAFx completion thread under the engine lock), state in the registered extractor (chosen) | One rule for CPU (serial and threaded) and every twin; no lock added to the engine; the derivation stays the shared function; a window completes as soon as the worker's SAD is in | A new optional callback on the internal extractor struct; without the VMAFx completion thread (plain libvmaf) a worker's frame becomes final at the next feeding call or read | Chosen |
| Advance from the worker that appended the SAD (frame listener) | Final at the worker's append, also for plain libvmaf | The registered extractor's state would be shared between workers and the feeding thread, with a lock of its own around read-derive-append | Not chosen: the completion thread already holds the engine lock |
| Derive in `extract()` / `collect()` right after the SAD append | No engine change for twins | With worker threads each worker has a private extractor and sees an arbitrary subset of frames: the state would need a lock shared across copies, or a collector-level critical section around read-derive-append | Not chosen: the engine already serialises the feeding thread |
| Recompute the carried state from the collector on each call (stateless advance) | No state struct | `prev_processed` would be recomputed from the stored `motion2` rather than carried as upstream carries it, and the start frame found by a scan per call (quadratic in long streams) | Not chosen: carry upstream's values |
| Keep the derivation at the flush and let windows over VMAF models wait | No change | Live windows (#2138, #2238) arrive after the stream ends; ruled out by Q-038 | Not chosen |
| A five-frame lookahead buffer in the window code instead of the extractor | Engine untouched | A second implementation of the window rule (HISS-19); the scores of a synchronous read would still be flush-only | Not chosen |
| Read back `motion_cuda`'s SAD every frame | Same one-frame lag as the other twins | Undoes the batching of ADR-0845 (one host synchronisation per frame); a performance decision for RC7 | Not chosen here: recorded as a follow-up |
| Move the three-frame paths of `motion_cuda` / `_sycl` / `_hip` onto the shared function | One derivation for every twin | Already incremental and exact; the deduplication is RC5 (`T-GPU-CUDA-HIP-DUPLICATED-KERNELS-2026-10-02`) | Not chosen in RC4 |

## Consequences

- **Positive**:
  - A window over a VMAF model completes one frame after its last, before
    the flush (`test_vmaf_window_completes_after_the_frame_after_last`:
    window [2, 5] of `vmaf_v0.6.1` open after the submit of frame 5, complete
    after the submit of frame 6, equal to a flushed session for every pooling
    method; `test_vmaf_window_completes_while_the_feeder_stalls`: with two
    worker threads the same window completes after frames 0 to 6 are
    submitted, with no further call and no flush); a per-frame metadata
    callback for a model fires one frame after its frame; `vmaf_score_pooled(i - 1, i - 1)` after
    `vmaf_read_pictures(i)` returns a score (the streaming pattern of
    Netflix#755).
  - Every value is the flush-time value. Per-frame `motion2` / `motion3`
    (every key of `motion` under four option sets, `motion_v2` under two,
    `float_motion`, and `vmaf_v0.6.1`) against master `5c32bde1f` built
    from the same flags: Netflix golden pair, both 1080p checkerboards,
    `sparks` 10-bit and BBB 4K (200 frames), each with default, AVX2 and
    scalar dispatch and with 0 and 4 worker threads: 32 988 values, 0
    different. The CUDA (RTX 4090), SYCL (Arc A380) and HIP (gfx1036) twins,
    named explicitly, against the CPU on the same fixtures at 0 and 4
    threads: 10 878 values each, 0 different. The parity gate's `motion`,
    `motion_debug`, `motion_mffw`, `motion_v2`, `motion_v2_mffw` and
    `float_motion` cells hold `==` on every backend and fixture.
  - A read of a fed frame no longer answers `-EINVAL` while a worker holds
    the frame.
- **Negative**:
  - With worker threads and no VMAFx window open (plain libvmaf, or a
    context without windows), a frame becomes final at the next feeding call
    or score read after the workers finished it, not at their append.
  - `motion_cuda` frames complete in batches of up to eight frames (its
    readback batch); a live window over a VMAF model on CUDA lags by up to
    nine frames.
  - `VmafFeatureExtractor` has one more callback; a descriptor copied whole
    (the Rust twin shim of the RC4 Rust lanes copies the C descriptor)
    inherits it and has to set or clear it.
- **Neutral / follow-ups**:
  - The Rust `motion_rust` twin (RC4 lane M) derives at its flush; it either
    implements `advance()` through the framework or clears the inherited
    callback (request `MI-1`, rc4-requests).
  - WP4 takes the completion-thread call (`advance_engine()`) and the
    window documentation into its branch (request `WP4-1`); WP9 drops the
    motion caveat of its `n_stats` request (request `WP9-2`).
  - `motion_cuda`'s readback batch against live latency is a tuning
    question for RC7.
  - The change is engine and extractor work independent of the VMAFx API:
    everything except the window test and its documentation can land on
    master on its own.

## References

- `Q-038` (maintainer decision, 2026-10-06, as relayed in the lane brief):
  "live VMAF windows are 1.0 scope (#2138 live n_stats, #2238 OBS-ready API),
  so motion2 / motion3 must be computed incrementally. Frame i's motion2 /
  motion3 become final as soon as frames 0..i+1 are scored, instead of all at
  flush, using the same statements and order so every value stays
  bit-identical to today's flush-time computation."
- [ADR-1478](1478-motion-five-frame-window-port.md) (the shared window
  function, frame retention), [ADR-1491](1491-gpu-motion-five-frame-window.md)
  (the five-frame window on the GPU twins),
  [ADR-2074](2074-vmafx-window-scores.md) (window scores; amended by rule 7),
  [ADR-0154](0154-score-pooled-eagain-netflix-755.md) (write-once scores,
  `-EAGAIN`), [ADR-0845](0845-cuda-motion-launch-overhead.md) (`motion_cuda`
  readback batch), [ADR-0795](0795-prev-ref-thread-safety.md)
  (worker-private extractor copies).
- `docs/state.md` `T-VMAFX-WINDOW-MOTION-AT-FLUSH-2026-10-06`,
  `T-ENGINE-READ-FED-FRAME-EINVAL-2026-10-06`.
- `Q-088` (maintainer popup 2026-10-07): accepted as written.
