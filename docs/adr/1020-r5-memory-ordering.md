<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1020: acq_rel memory ordering on ref-count decrement + mutex-destroy-after-unlock + picture-pool unlock ordering

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `correctness`, `threading`, `core`

## Context

A targeted concurrency audit (r5-memory-ordering) identified three HIGH-severity
races in core threading primitives:

1. **Ref-count decrement ordering** — `vmaf_ref_fetch_decrement` in `ref.c` and
   `ref.cpp` used `atomic_fetch_sub` (defaulting to `memory_order_seq_cst`), which
   is correct but needlessly strong. More critically, the pattern is documented
   as requiring `memory_order_acq_rel`: the *release* side ensures all prior stores
   from the decrementing thread are visible to others before the count drops; the
   *acquire* side ensures the last decrementer (the one that observes `old_cnt == 1`)
   sees all writes made by every other reference holder before it runs the destructor
   path (e.g. `picture.c` lines 230–233 dereferencing `priv`). Without the explicit
   `acq_rel`, the C11 standard makes no additional guarantee beyond the implicit
   seq_cst; making the intent explicit guards against future relaxation (compiler
   optimisation flags, LTO, architecture-specific codegen) and matches the canonical
   reference-counting idiom in C11 §7.17 / C++20 [atomics.ref.ops].

2. **Mutex-destroy-after-unlock race in feature_collector** —
   `vmaf_feature_collector_destroy` (feature_collector.cpp) acquires the lock,
   performs all teardown, unlocks, and *then* calls `pthread_mutex_destroy`. A
   concurrent thread blocked on `pthread_mutex_lock` at any of the five public entry
   points (append, get_score, find, set_aggregate, get_aggregate) could acquire the
   mutex in the window between unlock and destroy, and then operate on a partially
   freed struct — undefined behaviour under POSIX (destroying a locked mutex is UB;
   so is locking a destroyed mutex).

3. **picture_pool fetch: unlock before slot read** — `vmaf_picture_pool_fetch`
   (picture_pool.c) pops an index from the free list under the lock, then unlocks
   before copying `pool->pictures[idx]` into the caller's `*pic`. While no other
   thread can receive the same `idx` (it is off the free list), another thread
   executing `vmaf_picture_pool_close` could free the `pool->pictures` array in
   that window, making the dereference a use-after-free.

## Decision

1. Replace `atomic_fetch_sub(&ref->cnt, 1)` with
   `atomic_fetch_sub_explicit(&ref->cnt, 1, memory_order_acq_rel)` in both
   `core/src/ref.c` and `core/src/ref.cpp`. Add the required `using` declarations
   for `memory_order_*` constants to the C++ branch of `core/src/ref.h` so C++
   translation units resolve them without a `std::` prefix, mirroring the bare C11
   macros available in the C branch.

2. Add a `bool destroyed` field to `VmafFeatureCollector` (feature_collector.h).
   In `vmaf_feature_collector_destroy`, set `destroyed = true` under the lock
   immediately before `pthread_mutex_unlock`, then call `pthread_mutex_destroy`
   after the unlock. All five public entry points that acquire `lock` test
   `destroyed` immediately after locking: if true, they release the lock and return
   `-ENODEV` (or `nullptr` for pointer-returning functions).

3. In `vmaf_picture_pool_fetch`, copy `pool->pictures[idx]` to a stack-local
   `VmafPicture pic_snapshot` while still holding the lock, then unlock, then
   assign `*pic = pic_snapshot`. This bounds the data race on the shared array.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep seq_cst for ref decrement | Already correct per spec; no header change | Unnecessarily strong; intent opaque to code readers; acq_rel is the canonical idiom | acq_rel expresses intent precisely and may generate more efficient code on relaxed-memory architectures |
| Wrap feature_collector destroy in a reference count of its own | Race-free without a flag | Adds a second atomic; callers must drop their reference; invasive API change | No external callers hold VmafFeatureCollector references; the destroyed flag is simpler and equally safe |
| Move picture_pool unlock to after all post-pop work | No copy needed | Extends critical section through malloc + release-callback init; blocks other waiters longer | Copying one struct under the lock is cheaper than holding the lock through allocation |

## Consequences

- **Positive**: Eliminates three HIGH-severity data races. TSan builds will no
  longer flag these sites. Code now matches the documented C11/C++ canonical
  reference-counting pattern.
- **Negative**: `VmafFeatureCollector` gains one `bool` field (negligible size
  impact). Public entry points gain one branch each (cold path, no measurable
  throughput effect).
- **Neutral / follow-ups**: A TSan build (`meson setup build-tsan -Db_sanitize=thread`)
  can be used to verify the three sites are clean. No API surface changes.

## References

- C11 §7.17 / C++20 [atomics.ref.ops] — canonical acq_rel reference-counting pattern.
- POSIX pthread_mutex_destroy(3) — "destroying a locked mutex results in undefined
  behaviour"; locking a destroyed mutex also UB.
- r5-memory-ordering audit finding (HIGH × 3), 2026-06-04.
