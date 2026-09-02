<!-- markdownlint-disable MD013 -->
# ADR-0157: CUDA preallocation memory leak fix + `vmaf_cuda_state_free` public API (Netflix#1300)

- **Status**: Accepted
- **Date**: 2026-04-24
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: cuda, correctness, api, netflix-upstream, memory

## Context

Netflix upstream issue
[#1300](https://github.com/Netflix/vmaf/issues/1300) reports that
users running CUDA-accelerated VMAF in a loop — init → preallocate
pictures → fetch frames → close → (repeat every 30 frames) — see
**GPU memory rise monotonically** across cycles. Reporter matched the
`test_cuda_picture_preallocation_method_device` pattern and could
only avoid the leak by switching to the non-preallocating allocation
path.

Verification via `meson setup build-asan-cuda -Db_sanitize=address`

- running `test_cuda_pic_preallocation` confirmed **30 799 bytes
leaked in 28 allocations** across several distinct framework-side
paths. Code inspection identified the root causes:

1. **`VmafCudaState` struct ownership ambiguity** —
   `vmaf_cuda_state_init(&cu_state, cfg)` at
   [`core/src/cuda/common.c`](../../core/src/cuda/common.c)
   mallocs a `VmafCudaState`. `vmaf_cuda_import_state` at
   [`core/src/libvmaf.c`](../../core/src/libvmaf.c) **copies
   the struct by value** (`vmaf->cuda.state = *cu_state;`) but does
   **not** take ownership of the pointer. `vmaf_close` calls
   `vmaf_cuda_release(&vmaf->cuda.state)` on the copy — which
   `memset`s it to zero — but the original `cu_state` pointer
   returned to the caller is never freed. There was **no public
   `vmaf_cuda_state_free` API**; `vmaf_cuda_release` lives in the
   internal header
   [`core/src/cuda/common.h`](../../core/src/cuda/common.h)
   and is unavailable to callers. Per-cycle host-memory leak: one
   `VmafCudaState` struct (~80 bytes).

2. **`CudaFunctions` driver table never freed** —
   `vmaf_cuda_state_init` calls
   `cuda_load_functions(&c->f, NULL)` from `nv-codec-headers`,
   which dlopens `libcuda.so.1` and allocates a `CudaFunctions*`
   struct holding dlsym'd function pointers.
   `vmaf_cuda_release` destroys the CUDA stream + context but
   **never calls `cuda_free_functions(&cu_state->f)`**. Per-cycle
   host-memory leak: one `CudaFunctions` struct.

3. **`pthread_mutex_destroy` missing in ring-buffer close** —
   `vmaf_ring_buffer_close` at
   [`core/src/cuda/ring_buffer.c:80`](../../core/src/cuda/ring_buffer.c)
   **locks** `ring_buffer->busy`, frees the pictures, frees the
   buffer memory, but never **unlocks** or **destroys** the mutex.
   Destroying a locked mutex is POSIX UB; on glibc the mutex
   internals don't heap-allocate for default-initialized mutexes,
   but the UB is real and future glibc versions may add internal
   state that leaks.

4. **Cold-start error-path leak in `init_with_primary_context`** —
   discovered during fix: if
   `cuStreamCreateWithPriority` fails AFTER
   `cuDevicePrimaryCtxRetain` succeeds, the retained primary
   context is not released. Adjacent to #1/#2 and easier to fix in
   the same commit than to track separately.

Public-API design note: existing SYCL backend has the same shape
but declares ownership explicitly —
`vmaf_sycl_import_state()` documents that ownership is NOT
transferred and the caller must call `vmaf_sycl_state_free()`
after `vmaf_close()`. CUDA should match.

Earlier fork PRs — #60 / ADR-0122 (framesync segfault) and #62 /
ADR-0123 (null-guard) — hardened the CUDA path against NULL-state
dereferences but did not address the ownership leaks. PR #93 /
ADR-0156 (CHECK_CUDA graceful error propagation) is the prerequisite
for doing ownership cleanup correctly — error paths now propagate
instead of aborting, so "free on error" is actually reachable.

## Decision

Fix all four leak sources + introduce the missing public API, in one
PR:

### New public API

```c
/* core/include/libvmaf/libvmaf_cuda.h */

/**
 * Free VmafCudaState allocated by `vmaf_cuda_state_init()`.
 *
 * Must be called AFTER `vmaf_close()` on any VmafContext that
 * imported this state via `vmaf_cuda_import_state()`, because
 * `vmaf_close()` destroys the underlying CUDA stream and context.
 * Calling `vmaf_cuda_state_free()` first would leave `vmaf_close()`
 * with a dangling state.
 *
 * @param cu_state CUDA state to free. Safe to pass NULL.
 * @return 0 on success, or < 0 (a negative errno code) on error.
 */
int vmaf_cuda_state_free(VmafCudaState *cu_state);
```

Implementation in `core/src/cuda/common.c` is a NULL-safe
`free()` wrapper — `vmaf_close` / `vmaf_cuda_release` already
destroyed the stream, popped the context, and
`memset`'d the struct. The only remaining owned resource is the
heap allocation itself.

### `vmaf_cuda_release` frees the CudaFunctions table

In `vmaf_cuda_release`, save the `CudaFunctions*` pointer before the
existing `memset`, then call `cuda_free_functions(&f)` after the
`memset`. Order matters: `memset` first so `cu_state->f` is zero;
then free via the saved pointer. Avoids a dangling `f` in the
caller's struct if it inspects the state after close.

### Ring buffer close: unlock + destroy

`vmaf_ring_buffer_close` now does:

```c
err |= pthread_mutex_unlock(&ring_buffer->busy);
err |= pthread_mutex_destroy(&ring_buffer->busy);
free(ring_buffer->pic);
free(ring_buffer);
```

### Cold-start unwind in `init_with_primary_context`

On the `fail_after_pop` path, release the retained primary context
before returning. Also add an outer unwind in `vmaf_cuda_state_init`
so any failure in the inner init free both `c` and `c->f` cleanly.

### GPU-gated reducer test

[`core/test/test_cuda_preallocation_leak.c`](../../core/test/test_cuda_preallocation_leak.c)
— a 10-cycle reducer that does init → preallocate → fetch 10 pictures
→ close **with full cleanup on each cycle** (`vmaf_cuda_state_free`,
`vmaf_model_destroy`). GPU-gated: cycle 0 probes the driver; no
visible device → SKIP cleanly.

### Existing test cleanup

`test_cuda_pic_preallocation.c` and `test_cuda_buffer_alloc_oom.c`
— add the missing `vmaf_cuda_state_free(cu_state)` and
`vmaf_model_destroy(model)` calls after `vmaf_close(vmaf)`. Fixes
the test-side cleanup gap that masked the framework leaks before.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Fix all leaks + add `vmaf_cuda_state_free` (this ADR)** | Addresses every per-cycle leak surfaced by ASan; matches SYCL's explicit-ownership pattern; one coherent PR | New public API surface to maintain; larger diff than surgical | **Chosen** — user popup 2026-04-24 selected "Full fix: all leaks + public state_free API" |
| **Surgical: only `cuda_free_functions` + add state_free API** | Minimum to close Netflix#1300's CUDA-memory symptom | Leaves pthread mutex UB + cold-start leak unfixed; they'd surface as separate bugs later | Rejected via popup — user preferred wholesale |
| **Take ownership in `vmaf_cuda_import_state`** | No new API; existing callers don't change | Silent ownership transfer is surprising; violates POLA; inconsistent with SYCL's explicit-free pattern | Rejected — explicit > implicit for ownership |
| **Document + defer** | Zero code change | Users stay affected; Netflix#1300 OPEN since 2024 with no upstream response | Rejected — has a straightforward fix |

## Consequences

- **Positive**:
  - Netflix#1300 reproducer resolved: 10-cycle loop leaks zero
    framework bytes (183 bytes remain in `libcuda.so.1` internal
    state — per-process driver cache, not per-cycle; matches SYCL
    backend behaviour).
  - CUDA backend ownership story now matches SYCL: caller allocates
    state, framework copies-by-value in import, caller calls
    `state_free` after `vmaf_close`. Symmetric and documented.
  - Ring-buffer `pthread_mutex_destroy` closes a POSIX UB that
    future glibc versions could turn into a real crash.
  - Adjacent cold-start leak fixed in the same commit (retained
    primary context on stream-create failure).
  - ADR-0122 / ADR-0123 null-guards preserved verbatim; ADR-0156
    CHECK_CUDA_GOTO cleanup paths preserved; composable with the
    new free calls.
- **Negative**:
  - **New required step** for every CUDA caller: after
    `vmaf_close(vmaf)`, call `vmaf_cuda_state_free(cu_state)`.
    Callers who already do this (via informal `free(cu_state)`)
    will get a crash — double-free. Flagged under `### Changed` in
    CHANGELOG.
  - Public ABI growth by one symbol (additive; versioned via the
    shared library's symbol version script when release-please
    next cuts).
- **Neutral / follow-ups**:
  - Tests updated to demonstrate the full cleanup pattern. ffmpeg
    filter (`libavfilter/vf_libvmaf.c`) should be audited for the
    same cleanup sequence during the next ffmpeg-patches refresh
    — backlog follow-up.
  - If a self-hosted GPU runner lands (backlog T7-3), the new
    reducer gets real CI coverage instead of skipping at the
    driver-probe step.

## Verification

- `meson test -C core/build-cuda` → **40/40 pass** (was 39/39
  pre-PR + new reducer).
- `meson test -C build` (CPU-only) → **35/35 pass**.
- `ASAN_OPTIONS='detect_leaks=1:leak_check_at_exit=1'
  build-asan-cuda/test/test_cuda_preallocation_leak` → **183 bytes
  leaked in 4 allocations**, **all in `libcuda.so.1` driver
  internal state** (cuInit cache — persists for process lifetime,
  does NOT grow per cycle; verified by N=1 vs N=10 comparison).
  **Zero `core/src/*` frames** in the leak traces.
- `clang-tidy -p build-cuda --quiet <5 touched files>` → **exit 0**.
- CI-equivalent `clang-tidy -p build --quiet
  core/include/libvmaf/libvmaf_cuda.h` (the only CI-visible
  file after the ADR-0156 exclusion filter) → **exit 0**.
- `pre-commit run --files <touched>` → all hooks pass.
- Reducer verified to exercise the fix: pre-fix (with the sweep
  reverted), ASan reports the `VmafCudaState` malloc + the
  `CudaFunctions` dlopen table as leaked per cycle. Post-fix
  those stack frames are gone.

## References

- Upstream issue:
  [Netflix/vmaf#1300](https://github.com/Netflix/vmaf/issues/1300)
  ("CUDA-VMAF Memory Leak (preallocation method) in libvmaf"),
  OPEN since 2024; no maintainer fix as of 2026-04-24.
- [ADR-0122](0122-cuda-framesync-segfault-hardening.md) — fork PR
  #60 CUDA framesync hardening (preserved).
- [ADR-0123](0123-cuda-null-guard.md) — fork PR #62 null-guard
  (preserved).
- [ADR-0156](0156-cuda-graceful-error-propagation-netflix-1420.md)
  — CHECK_CUDA graceful error propagation (prerequisite; enables
  the new error-path cleanup to actually run).
- [ADR-0141](0141-touched-file-cleanup-rule.md) — touched-file
  lint rule.
- [rebase-notes 0050](../rebase-notes.md) — upstream-sync
  invariants for this decision.
- Backlog: `.workingdir2/BACKLOG.md` T1-7.
- User direction 2026-04-24 popup: "T1-7 CUDA preallocation leak
  Netflix#1300" → "Full fix: all leaks + public state_free API".
