<!-- markdownlint-disable MD013 MD060 -->
# ADR-0795: Clarify and harden VmafFeatureExtractor.prev_ref thread-safety invariant

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: threading, feature-extractor, batch-threading, correctness

## Context

`VmafFeatureExtractor.prev_ref` is a `VmafPicture` field written by the dispatch
layer immediately before calling an extractor's `extract()` callback and cleared
immediately after.  The field exists because the `VMAF_FEATURE_EXTRACTOR_PREV_REF`
protocol (used by `integer_motion_v2`) requires the framework to inject the previous
reference frame into the extractor struct rather than passing it as a parameter.

A PR #115 thread-safety audit flagged the pattern as a potential data race when
multiple `BATCH_THREADING` workers write `fex->prev_ref` concurrently.  The audit
recommended hoisting `prev_ref` out of `VmafFeatureExtractor` and into
`BatchThreadData` (the per-thread TLS data structure) as recommendation #4.

**Actual race analysis**:

1. **BATCH_THREADING path** (`threaded_extract_batch_func`): each thread has its
   own `BatchThreadData *td` (lazy-allocated in thread-pool TLS, never shared).
   `td->fex_ctx[i]` is created via `vmaf_feature_extractor_context_create`, which
   `memcpy`s the registered extractor into a new heap object.  Therefore
   `td->fex_ctx[i]->fex != registered_fex_ctx[i]->fex` — different heap objects.
   The `prev_ref` write goes to the per-thread copy, not the shared registered fex.
   No race.

2. **Non-batch pool path** (`threaded_extract_func`): each pool slot owns its own
   deep-copied `VmafFeatureExtractor`.  The pool serialises access (one thread holds
   a slot at a time via `in_use`).  The `prev_ref` write and clear are entirely
   within the holding thread's critical section.  No race.

3. **Sequential path** (`read_pictures_dispatch_one`): called only when
   `read_pictures_should_skip` returns false.  With `thread_pool != NULL`,
   `read_pictures_should_skip` returns true for all non-CUDA/SYCL non-TEMPORAL fex,
   so `read_pictures_dispatch_one` never writes `prev_ref` on any fex that is also
   dispatched to the batch/pool thread paths.  No race.

Despite the absence of an active race, the code was fragile: the `fex` variable in
`threaded_extract_batch_func` aliased the **shared** registered extractor pointer
with no `const` annotation, the write used the same variable name as the read-only
flag checks, and there was no assertion enforcing that the per-thread copy was
distinct from the shared registration.

## Decision

Apply a defensive hardening in `core/src/libvmaf.c` without changing the extractor
API (`VmafFeatureExtractor.prev_ref` is kept as the injection point):

1. In `threaded_extract_batch_func`: rename the local `fex` (which pointed to the
   **shared** registered extractor) to `shared_fex` and mark it `const`.  Add an
   `assert(td->fex_ctx[i]->fex != shared_fex)` to enforce at runtime that the
   per-thread context's fex is a distinct heap object.  Add a comment citing this
   ADR.

2. In `threaded_extract_func`: add a comment explaining why the pool-slot-owned
   `fex->prev_ref` write is safe (exclusive pool acquisition + deep copy at slot
   creation time).

These changes are documentation + assertion only — no semantic change.  They make
the invariant machine-checked (assert fires in debug builds if the deep-copy
contract is broken by a future refactor) and human-visible (const qualifier, rename,
comments).

## Alternatives considered

| Option | Pros | Cons |
|--------|------|------|
| Add `prev_ref` array to `BatchThreadData` indexed by extractor slot | Completely eliminates `fex->prev_ref` writes in the batch path | Requires allocating a `VmafPicture[cnt]` per thread; complicates lifecycle of picture refs; extractor still needs `fex->prev_ref` for the sequential path |
| Pass `prev_ref` as a new `extract()` parameter | Eliminates the struct-field injection entirely | Breaks the `VmafFeatureExtractor` public API contract; requires changing every extractor's `extract` callback and all callers |
| Add a mutex around `fex->prev_ref` reads/writes | Provably safe | Unnecessary — no concurrent writes exist today; adds lock overhead on the hot-path |
| Current approach (rename + const + assert + comments) | Zero runtime overhead; machine-checked invariant; no API change | Does not prevent the race if the deep-copy contract is broken silently |

The chosen approach (const rename + assert + comments) was preferred because the
race does not actually exist in the current code and a full API change is not
justified.  The assert provides a regression guard for the underlying deep-copy
invariant.

## Consequences

**Positive**:

- The `assert` in `threaded_extract_batch_func` fires in debug builds if
  `vmaf_feature_extractor_context_create` ever returns a context sharing its `fex`
  pointer with the registered object.
- `const VmafFeatureExtractor *shared_fex` makes it a compile-time error to
  accidentally write through the shared pointer.
- Comments reduce re-investigation cost when future engineers audit threading.

**Negative**:

- None.  This is a documentation + assertion change only.

**Neutral follow-ups**:

- The fuller recommendation #4 (move `prev_ref` into `BatchThreadData` as a staged
  slot) remains open as a future refactor once the extractor API is versioned
  (VMAFX Phase 4 / ADR-0709).

## References

- PR #115 thread-safety audit thread (recommendation #4: hoist `prev_ref` into
  `BatchThreadData`)
- `core/src/libvmaf.c` — `threaded_extract_batch_func`, `threaded_extract_func`
- `core/src/feature/integer_motion_v2.c` — only PREV_REF consumer in the default
  model; reads `fex->prev_ref.data[0]` and `fex->prev_ref.stride[0]`
- ADR-0795 (this document)
