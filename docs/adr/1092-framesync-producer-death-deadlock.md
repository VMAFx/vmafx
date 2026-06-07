<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1092: framesync producer-death deadlock — abort flag + shutdown broadcast

- **Status**: Accepted
- **Date**: 2026-06-07
- **Deciders**: Lusoris
- **Tags**: `core`, `threading`, `correctness`, `sanitizer`, `fork-local`

## Context

`VmafFrameSyncContext` couples a producer thread (calling
`vmaf_framesync_acquire_new_buf` + `vmaf_framesync_submit_filled_data`) with a
consumer thread (calling `vmaf_framesync_retrieve_filled_data`).

`retrieve_filled_data` contains an unbounded `while` loop that calls
`pthread_cond_wait` whenever the requested frame index is not yet in BUF_FILLED
state.  There was no way to interrupt this wait.  If the producer thread died —
from an OOM in `acquire_new_buf`, from an error returned by the feature
extractor's `extract()` callback, or from an unhandled early exit — the
consumer would wait forever.  Because `vmaf_thread_pool_wait` in `vmaf_close`
cannot return until all worker threads finish, the entire process would hang.

As a consequence, calling `vmaf_framesync_destroy` while a consumer was still
blocked in `pthread_cond_wait` is undefined behaviour per POSIX
(`pthread_cond_destroy` with waiters: UB).

The `SAN-FRAMESYNC-MUTEX-DOMAIN` fix (PR #548, ADR referenced at state.md line
604) established the strict M0-before-M1 lock ordering that prevents TSan races.
That fix correctly handles the normal producer–consumer handshake.  The
producer-death path was not addressed.

## Decision

Add an `aborted` (plain `int`) field to `VmafFrameSyncContext`.  Add a new
function `vmaf_framesync_abort(fs_ctx)` that:

1. Takes `retrieve_lock` (M1).
2. Sets `fs_ctx->aborted = 1`.
3. Broadcasts on the `retrieve` condvar.
4. Releases M1.

Update `retrieve_filled_data` to check `aborted` in two places:

- Before entering `pthread_cond_wait` (persistent flag covers the case where
  abort fired before cond_wait was reached).
- Immediately after waking from `pthread_cond_wait` (covers the case where
  abort's broadcast was the wake signal).

Both checks return `-ECANCELED` with all locks released.

Add a safety-net `vmaf_framesync_abort` call at the top of
`vmaf_framesync_destroy` so that any consumer still sleeping when destroy is
called receives a broadcast before the condvar is destroyed — preventing the
POSIX UB even if the caller forgot to drain threads first.

`vmaf_framesync_abort` is declared in `framesync.h` so callers (feature
extractors, libvmaf.c error paths) can invoke it directly on producer failure.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| `pthread_cancel` + cancellation points | No new API | Non-deterministic; interacts badly with cleanup handlers; cancellation state is thread-scoped, not context-scoped; POSIX discourages it in production code | Not chosen |
| Timed wait (`pthread_cond_timedwait`) | No new API surface | Adds latency on the happy path; retry interval is a policy decision with no right answer; does not actually fix the root cause | Not chosen |
| Abort flag only (no broadcast in destroy) | Simpler destroy | Still leaves a POSIX UB window if a waiter is in flight when destroy is called | Not chosen — defence-in-depth broadcast kept |
| C11 `_Atomic int` for the flag | Formally cleaner memory model | Not needed: POSIX guarantees that all writes made while holding a mutex are visible to any thread that subsequently acquires the same mutex; `retrieve_lock` is always held when reading/writing `aborted` | Not chosen |

## Consequences

- **Positive**: producer-death no longer causes an infinite hang or POSIX UB in
  `vmaf_framesync_destroy`; TSan / helgrind will see a clean condvar lifecycle.
- **Positive**: consumers get a well-defined `-ECANCELED` error code they can
  propagate up the call stack.
- **Negative**: callers that want to surface the abort to the user must check for
  `-ECANCELED` in their `retrieve_filled_data` return-value handling.
- **Neutral**: the `aborted` field adds 4 bytes to `VmafFrameSyncContext`
  (internal struct, no ABI impact).
- **Neutral**: feature extractor error paths should call `vmaf_framesync_abort`
  before returning; the `vmaf_framesync_destroy` safety-net broadcast is
  defence-in-depth, not the primary signal path.

## References

- `SAN-FRAMESYNC-MUTEX-DOMAIN` fix: PR #548 (2026-05-09), state.md line 604.
- POSIX `pthread_cond_destroy`: "Attempting to destroy a condition variable upon
  which other threads are currently blocked results in undefined behavior."
- `feedback_no_test_weakening`: fix the implementation, not the gate.
- Internal analysis: `framesync.c:retrieve_filled_data` infinite loop on
  producer death — identified 2026-06-07 by framesync-deadlock-paths agent.
