# ADR-0778: Picture pool / framesync lifecycle audit and targeted fixes

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `correctness`, `picture-pool`, `framesync`, `refcount`,
  `gpu`, `sycl`, `audit`, `fork-local`

## Context

A systematic audit of the picture pool and framesync lifecycle across all
backends (CPU, CUDA, SYCL, HIP, Metal) was requested to identify refcount
inversions, GPU in-flight frees, unbounded pool growth, concurrent access
gaps, and frame-ordering bugs. The audit covers:

- `core/src/picture.c` and `core/src/picture_pool.c` (CPU pool)
- `core/src/framesync.c`
- `core/src/gpu_picture_pool.c` (backend-agnostic GPU pool)
- `core/src/cuda/picture_cuda.c`
- `core/src/sycl/picture_sycl.cpp`
- `core/src/hip/picture_hip.c`
- `core/src/metal/picture_metal.mm` and `core/src/metal/picture_import.mm`
- The `prev_ref` dispatch path in `core/src/libvmaf.c`

Full findings are in the companion research digest:
`docs/research/picture-pool-framesync-lifecycle-audit-2026-05-29.md`.

## Decision

Fix the two confirmed bugs in this PR. Track the two medium-severity
non-trivial issues as follow-up work.

**Fix A** (`read_pictures_dispatch_one`, severity HIGH): replace the bare
struct copy of `vmaf->prev_ref` into `fex->prev_ref` with a proper
`vmaf_picture_ref` call, mirroring the already-correct pattern in the
threaded path (`threaded_extract_func`, line 1673). Add a matching
`vmaf_picture_unref` after extract returns.

**Fix E** (`pool_preallocate_pictures` error path, severity MEDIUM): the
unwind loop calls `vmaf_picture_unref` on pictures whose `priv` and `ref`
have already been stripped to `NULL`, causing `vmaf_picture_unref` to
return `-EINVAL` and leak the underlying buffer. Fix by restructuring
the loop so stripping happens only after the full allocation succeeds, or
by recording the raw pointer separately for the unwind.

**Track B** (SYCL pool close without synchronisation): the SYCL
`sycl_pool_free_cb` does not call any USM queue drain before freeing
device buffers. Adding a `synchronize_picture_callback` slot for SYCL
requires touching the SYCL extractor queue-lifecycle API; deferred to a
follow-up issue.

**Track D** (CPU pool close has no timeout): the `pthread_cond_wait`
loop in `vmaf_picture_pool_close` blocks indefinitely if a caller leaks
a pool reference. A timeout or reference-leak diagnostic is a defensive
improvement but not a correctness regression today; deferred.

## Alternatives considered

**Fix all five findings in one PR** — maximises coverage, but the SYCL
sync-callback change requires a larger API discussion and D is cosmetic.
Rejected: would block the HIGH-severity fix on an unrelated design point.

**Fix only Finding A** — minimal risk, but leaves the pool-preallocate
buffer leak (E) unfixed. Rejected: E is equally mechanical and should
land together.

**No fix, document-only** — zero code risk, but the HIGH-severity UAF
window stays open. Not acceptable for a correctness repo.

## Consequences

- **Positive**: Eliminates the use-after-free window in the synchronous
  non-GPU `prev_ref` path; fixes the silent buffer leak in
  `pool_preallocate_pictures` error unwind.
- **Negative**: Slightly more refcount traffic on the synchronous path
  (one `vmaf_picture_ref` + one `vmaf_picture_unref` per frame per
  `PREV_REF` extractor).
- **Neutral / follow-ups**:
  - Open tracking issue for Finding B (SYCL pool close sync).
  - Open tracking issue for Finding D (pool close timeout).
  - The framesync list growth (Finding C) is a footprint concern, not a
    bug; no immediate action required.

## References

- `docs/research/picture-pool-framesync-lifecycle-audit-2026-05-29.md`
- `core/src/libvmaf.c` lines 1673-1686 (correct threaded model)
- `core/src/picture_pool.c` lines 82-105 (Finding E site)
- `core/src/libvmaf.c` line 2312 (Finding A site)
- Netflix/vmaf upstream issue #1300 (mutex destroy fix, prior art)
