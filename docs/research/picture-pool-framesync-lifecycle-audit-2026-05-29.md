<!-- Copyright 2026 Lusoris — BSD-3-Clause-Plus-Patent -->
# Picture pool / framesync lifecycle audit — 2026-05-29

Scope: `core/src/picture.c`, `core/src/picture_pool.c`,
`core/src/framesync.c`, `core/src/gpu_picture_pool.c`,
`core/src/cuda/picture_cuda.c`, `core/src/sycl/picture_sycl.cpp`,
`core/src/hip/picture_hip.c`, `core/src/metal/picture_metal.mm`,
`core/src/metal/picture_import.mm`, and the `prev_ref` paths in
`core/src/libvmaf.c`.

Companion ADR: [ADR-0778](../adr/0778-picture-pool-framesync-audit.md).

---

## 1  Findings summary

### Finding A — PREV_REF aliased without refcount (HIGH)

**File:** `core/src/libvmaf.c:2312`

```c
fex_ctx->fex->prev_ref = vmaf->prev_ref;   // shallow struct copy, no vmaf_ref_fetch_increment
```

`vmaf->prev_ref` holds a reference to the previous frame. The threaded
path (`threaded_extract_func`, line 1673) calls `vmaf_picture_ref` to
obtain its own counted reference before handing off to the worker. The
synchronous non-GPU path (`read_pictures_dispatch_one`) performs a bare
struct copy instead. If the frame arrives out-of-band or the extractor
takes a non-trivial amount of time (e.g., a CPU extractor dispatched
from the double-buffer path), `vmaf->prev_ref` can be replaced and
decremented by the next `read_pictures_update_prev_ref` call before the
extractor has read `prev_ref.data`. This is a use-after-free window
rather than a guaranteed crash: `vmaf->prev_ref` usually holds a pool
buffer rather than a heap allocation, so the underlying storage survives
past the refcount drop — but the refcount can reach zero and cause
`pooled_picture_release` to return the buffer to the pool, where a
subsequent frame's `picture_pool_fetch` reuses it, corrupting the data
the extractor is reading.

The threaded path (line 1673–1681) and the CUDA batch path (line 2446)
are **not** affected: they call `vmaf_picture_ref` before publishing to
the worker or set `fex->prev_ref` under a lock and clear it before
allowing further progress.

The non-GPU synchronous dispatch in `read_pictures_dispatch_one` is the
only site that omits the `vmaf_picture_ref` / `vmaf_picture_unref`
bracket.

**Fix sketch** (trivial):

```c
if ((fex_ctx->fex->flags & VMAF_FEATURE_EXTRACTOR_PREV_REF) && vmaf->prev_ref.ref) {
    vmaf_picture_ref(&fex_ctx->fex->prev_ref, &vmaf->prev_ref);  // was bare struct copy
}
// ... extract() ...
if (fex_ctx->fex->flags & VMAF_FEATURE_EXTRACTOR_PREV_REF) {
    if (fex_ctx->fex->prev_ref.ref)
        vmaf_picture_unref(&fex_ctx->fex->prev_ref);
    memset(&fex_ctx->fex->prev_ref, 0, sizeof(fex_ctx->fex->prev_ref));
}
```

---

### Finding B — `vmaf_gpu_picture_pool_close` frees without sync (MEDIUM)

**File:** `core/src/gpu_picture_pool.c:85–110`

The CUDA back-pressure model relies on each picture's `cuda.finished`
event being queried through `vmaf_cuda_picture_synchronize` before the
buffer is reused (via `vmaf_gpu_picture_pool_fetch`'s
`synchronize_picture_callback`). However, `vmaf_gpu_picture_pool_close`
calls the `free_picture_callback` array unconditionally and immediately:

```c
for (unsigned i = 0; i < pool->cfg.pic_cnt; i++) {
    err |= pool->cfg.free_picture_callback(&pool->pic[i], pool->cfg.cookie);
}
```

`vmaf_cuda_picture_free` calls `cuStreamSynchronize` before freeing
device memory, so the CUDA variant is safe on its own. The SYCL
`sycl_pool_free_cb` does **not** call any synchronisation primitive
before freeing USM buffers. If teardown races with an in-flight SYCL
kernel, the kernel writes to freed device memory.

The CPU `VmafPicturePool` (`picture_pool.c`) blocks in
`vmaf_picture_pool_close` with a `pthread_cond_wait` loop until
`free_list_top == pic_cnt`, so it is clean.

**Fix sketch:** add a `synchronize_picture_callback` hook for SYCL
(currently `nullptr`), calling `vmaf_sycl_queue_wait` or equivalent
before freeing USM, and invoke it in `vmaf_gpu_picture_pool_close`
before `free_picture_callback`.

---

### Finding C — framesync list grows unbounded (severity: LOW)

**File:** `core/src/framesync.c:129–156`

`vmaf_framesync_acquire_new_buf` appends a new node to the linked list
whenever all existing nodes are occupied. The list never shrinks:
`vmaf_framesync_release_buf` marks a node `BUF_FREE` rather than
deleting it, so released nodes are reused on the next acquire. In
steady-state (same concurrency level throughout the run) this is fine.
Under a burst of concurrent acquires — which can happen during
per-feature extractor initialisation with many parallel workers — the
list grows to the burst watermark and stays there. No O(n) scan is done
at runtime outside the lock, so this is a memory footprint concern
rather than a correctness bug. At typical VMAF concurrency levels
(≤16 worker threads) the list never exceeds a few dozen nodes.

---

### Finding D — `vmaf_picture_pool_close` does not have a timeout (severity: LOW)

**File:** `core/src/picture_pool.c:167–193`

The close path blocks indefinitely:

```c
while (pool->free_list_top < pool->cfg.pic_cnt) {
    pthread_cond_wait(&pool->available, &pool->lock);
}
```

If a caller holds a reference to a pool picture and errors out without
unreffing it, `vmaf_picture_pool_close` deadlocks. This is a defensive
concern; the current callers always unref, but no timeout protects
against future misuse.

---

### Finding E — `pool_preallocate_pictures` error path leaks buffers (MEDIUM)

**File:** `core/src/picture_pool.c:82–105`

The pre-allocation loop allocates pictures with `vmaf_picture_alloc`,
then immediately strips `priv` and `ref` from the struct:

```c
free(p->pictures[i].priv);
vmaf_ref_close(p->pictures[i].ref);
p->pictures[i].priv = NULL;
p->pictures[i].ref = NULL;
```

The error-unwind path two lines above calls `vmaf_picture_unref` on
already-allocated pictures:

```c
for (unsigned j = 0; j < i; j++) {
    vmaf_picture_unref(&p->pictures[j]);
}
```

At unwind iteration `j`, `p->pictures[j].priv == NULL` and
`p->pictures[j].ref == NULL` (the strip already ran for all `j < i`).
`vmaf_picture_unref` checks `pic->ref` at the top:

```c
if (!pic->ref)
    return -EINVAL;
```

It returns `-EINVAL` rather than crashing, so the buffer allocated by
`vmaf_picture_alloc` at index `j` is **leaked**: `free(pic->priv)` and
the `release_picture` callback (which returns `data[0]` to the static
pool or calls `aligned_free`) are never reached.

**Fix:** do not strip `priv`/`ref` before the end of the loop succeeds.
Store the raw data pointer separately, or restructure the loop so
stripping happens only after successful full allocation.

---

### Findings not confirmed

- **Concurrent pool access without locks (CPU pool):** `pic_pool_acquire`
  and `pic_pool_release` in `picture.c` both hold `pic_pool.lock` for
  their entire body. `VmafPicturePool` (picture_pool.c) uses its own
  `pthread_mutex_t lock`. `VmafGpuPicturePool` uses `pool->busy`. All
  three paths are correctly serialised.
- **Returns picture to pool while still in flight on GPU (CUDA):**
  `vmaf_cuda_picture_free` calls `cuStreamSynchronize` before freeing;
  `vmaf_gpu_picture_pool_fetch` calls `synchronize_picture_callback`
  before re-issuing. The CUDA path is safe. The SYCL path has the gap
  documented in Finding B.
- **Refcount decrement before use:** `vmaf_picture_unref` in `picture.c`
  decrements first, then checks `old_cnt == 1` before calling the release
  callback — this is the standard fetch-then-compare pattern and is correct.
- **Metal import ring:** `vmaf_metal_state_build_pictures` transfers
  ownership atomically (struct copy + zero-fill + clear `pending`), so the
  ring slot cannot be re-used until `vmaf_read_pictures` unrefs both
  pictures. The ring depth (2 slots) matches the serial FFmpeg dispatch
  contract documented in the header.

---

## 2  Severity classification

| ID | Severity | Class                          | Fixed |
|----|----------|--------------------------------|-------|
| A  | HIGH     | Refcount inversion / UAF       | Yes   |
| B  | MEDIUM   | GPU in-flight free (SYCL)      | No    |
| C  | LOW      | Framesync list footprint       | No    |
| D  | LOW      | Pool close deadlock on leak    | No    |
| E  | MEDIUM   | Error-path buffer leak         | Yes   |

---

## 3  References

- ADR-0778 decision record
- `core/src/libvmaf.c` lines 1673-1686 (threaded path — correct reference model)
- `core/src/gpu_picture_pool.c` lines 85-110
- `core/src/picture_pool.c` lines 82-105
- Netflix/vmaf upstream issue #1300 (mutex destroy fix, already merged)
