<!-- markdownlint-disable MD060 -->
# Research-0047 — CUDA graph capture feasibility for the per-frame submit chain

| Field             | Value                                                                |
|-------------------|----------------------------------------------------------------------|
| Date              | 2026-05-02                                                           |
| Companion ADR     | None (decision is DEFER + lean NO-GO; documented here, no ADR)       |
| Author            | cuda-graph-capture worktree (Lusoris fork)                           |
| Trigger           | `cuda-dedup-profile-2026-05-02.md` §Optimization candidates item 5   |
| Worktree branch   | `feat/cuda-graph-capture-investigation`                              |

## Question

The 2026-05-02 dedup profile flagged **CUDA graph capture for the per-frame
submit-readback chain** as the highest-potential optimization for the CUDA
backend, with an estimated 10–20% gain and the highest implementation
complexity. The `kernel_template.h` docstring already calls out
`VmafCudaKernelLifecycle.finished` as a hook point left open for graph
capture.

Question: does a CUDA graph capture path actually buy the predicted 10-20%
gain on the fixed submit→readback chain, and at what code-complexity cost?

## TL;DR

**DEFER, leaning NO-GO.** The 10-20% prediction was an order-of-magnitude
guess that conflated "submit-side CPU overhead" with "wall-clock fps gain."
The dedup profile actually shows the submit-side libvmaf CPU samples
(`submit_fex_cuda` + descendants) totalling ~3-5% of all CPU samples. Even
a perfect graph-capture submit caps the realised gain at ~3-5%, not 10-20%.
At the same time, the picture-pool round-robin (4 slots) defeats
single-graph capture: each pool slot has its own
(`picture_stream`, `dist_ready_event`) tuple, forcing one captured graph
*per slot per extractor* and per-frame `cuGraphExecKernelNodeSetParams`
rebinding for the kernel's `(ref, dis)` device-pointer arguments. The
rebind is itself a driver call.

Cheaper, equally-or-more-impactful wins exist (profile items 1-3): fence
batching across co-scheduled extractors (~8-12%), eliminating the
synchronous wait inside `upload_plane_cuda` (~5%), and per-frame pinned-host
buffer reuse (~2-3%). Their cumulative ~15-20% gain dwarfs graph capture's
realistic ceiling, with materially less code complexity.

Recommendation: do not build a CUDA graph-capture path now. Revisit only
**after** profile items 1-3 have shipped and a fresh profile has confirmed
the submit-chain is still the dominant residual.

## Context

The per-frame CUDA lifecycle (per `cuda/kernel_template.h`) is:

```text
submit():
    cuMemsetD8Async(rb->device, 0, bytes, lc->str)                       (i)
    cuStreamWaitEvent(picture_stream, dist_ready_event, 0)               (ii)
    cuLaunchKernel(func, grid, block, 0, picture_stream, params)         (iii)
    cuEventRecord(lc->submit, picture_stream)                            (iv)
    cuStreamWaitEvent(lc->str, lc->submit, 0)                            (v)
    cuMemcpyDtoHAsync(host_pinned, device, bytes, lc->str)               (vi)
    cuEventRecord(lc->finished, lc->str)                                 (vii)

collect():
    cuStreamSynchronize(lc->str)                                         (viii)
```

Steps (i)-(vii) are the candidate "fixed shape" graph. Step (viii) is the
host stall point, which graph capture cannot remove (replay still has to
synchronize at some point).

## What's actually fixed vs. parameterised

Walking the chain against `integer_psnr_cuda.c` and `picture_cuda.c`:

| Step  | Inputs                                       | Fixed across frames? |
|-------|----------------------------------------------|----------------------|
| (i)   | `rb->device` ptr, `bytes`, `lc->str`         | **Yes** — fixed at `init()` |
| (ii)  | `picture_stream`, `dist_ready_event`         | **No** — pool-slot dependent |
| (iii) | `func`, grid, block, `(ref, dis, sse, w, h)` | **Mixed** — `func` fixed, `(ref, dis)` change per frame, `(w, h)` fixed within a session |
| (iv)  | `lc->submit`, `picture_stream`               | **No** — `picture_stream` is pool-slot dependent |
| (v)   | `lc->str`, `lc->submit`                      | **Yes** |
| (vi)  | `host_pinned`, `rb->device`, `bytes`, `lc->str` | **Yes** |
| (vii) | `lc->finished`, `lc->str`                    | **Yes** |

The picture pool default depth is `pic_cnt = 4` for CUDA
(`core/src/libvmaf.c:247`). Each pool slot owns its own
`CUstream picture_stream` and `CUevent dist_ready_event`, exposed via
`vmaf_cuda_picture_get_stream(pic)` and
`vmaf_cuda_picture_get_ready_event(pic)`. The slot rotates per frame
(`pool->curr_idx = (pool->curr_idx + 1) % pic_cnt` in
`gpu_picture_pool.c`).

Two consequences:

1. A single captured graph cannot replay across pool slots — its
   `picture_stream` and `dist_ready_event` are baked into the captured
   nodes. We'd need **N captured graphs per extractor** where N is the
   pool depth (typically 4).
2. The kernel-launch parameters `(ref_pic, dis_pic)` change per frame
   even **within the same pool slot** — `vmaf_cuda_picture_get` returns
   different device-pointer values across frames. We'd need
   `cuGraphExecKernelNodeSetParams` per frame to rebind those pointers
   on the captured launch node before each `cuGraphLaunch`.

So the prototype shape is not "capture once, replay N times." It is
"capture once per pool slot, then per-frame rebind kernel params on the
correct slot's graph and launch it." That's substantially closer to the
existing path than the slogan "fixed shape, just replay it" suggests.

## API sketch (what would have to change)

If we were to ship this, the kernel-template extension would look roughly
like:

```c
/* New struct: per-(extractor, pool-slot) captured graph + the
 * launch-node handle so we can rebind kernel params per frame. */
typedef struct VmafCudaKernelGraph {
    CUgraph         graph;          /* template, instantiated once */
    CUgraphExec     exec;           /* the executable instance */
    CUgraphNode     launch_node;    /* kernel launch node — for SetParams */
    CUDA_KERNEL_NODE_PARAMS params; /* mutable copy for rebinding */
} VmafCudaKernelGraph;

/* New helper: capture the fixed-shape submit chain on lc->str.
 * Caller passes the picture-stream + dist-event tuple bound to a
 * specific pool slot. */
int vmaf_cuda_kernel_graph_capture(VmafCudaKernelGraph    *g,
                                   VmafCudaKernelLifecycle *lc,
                                   VmafCudaKernelReadback  *rb,
                                   VmafCudaState           *cu_state,
                                   CUstream                picture_stream,
                                   CUevent                 dist_ready_event,
                                   CUfunction              func,
                                   /* placeholder kernel params: */
                                   void                  **kernel_param_template,
                                   int                     n_params,
                                   unsigned                grid_x,
                                   unsigned                grid_y,
                                   unsigned                block_x,
                                   unsigned                block_y);

/* Per-frame rebind + launch. */
int vmaf_cuda_kernel_graph_launch(VmafCudaKernelGraph *g,
                                  VmafCudaState       *cu_state,
                                  void               **kernel_params,
                                  CUstream             launch_stream);

/* Teardown. */
int vmaf_cuda_kernel_graph_destroy(VmafCudaKernelGraph *g,
                                   VmafCudaState       *cu_state);
```

Per-extractor consumer pattern (PSNR only sketched here):

```c
typedef struct PsnrStateCuda {
    VmafCudaKernelLifecycle  lc;
    VmafCudaKernelReadback   rb;
    /* NEW: one captured graph per pool slot. */
    VmafCudaKernelGraph      graph[VMAF_GPU_PICTURE_POOL_MAX];
    bool                     graph_captured[VMAF_GPU_PICTURE_POOL_MAX];
    /* ... existing fields ... */
} PsnrStateCuda;

static int submit_fex_cuda(...)
{
    const unsigned slot = vmaf_cuda_picture_get_pool_slot(ref_pic);
    if (!s->graph_captured[slot]) {
        /* First frame on this pool slot: capture lazily.
         * Cost: ~1 frame-time hit for cuStreamBeginCapture +
         * cuGraphInstantiate. */
        ...capture path...
        s->graph_captured[slot] = true;
    }
    /* Steady state: rebind (ref, dis) pointers + launch. */
    void *kernel_params[] = {(void *)ref_pic, (void *)dist_pic,
                             (void *)s->rb.device, &s->frame_w, &s->frame_h};
    return vmaf_cuda_kernel_graph_launch(&s->graph[slot], fex->cu_state,
                                         kernel_params,
                                         vmaf_cuda_picture_get_stream(ref_pic));
}
```

### Complexity estimate

| Item                                                     | LOC (est.) |
|----------------------------------------------------------|------------|
| `kernel_template.h`: 3 new helpers + struct + docstrings | +180       |
| Per-feature: capture-on-first-frame + slot-array state   | +60 / feature |
| Per-feature: pool-slot retrieval API on `picture_cuda`   | +20 (one-shot) |
| Migration of 12 existing CUDA features (long-tail)       | +60 × 12 = +720 |
| Tests: per-feature graph-replay parity (places=4) gate   | +120       |
| Docs: `docs/backends/cuda-graph-capture.md`              | +200       |
| **Total** (project-wide rollout)                         | **~1240**  |
| **Total** (PSNR-only opt-in, single-feature scope)       | **~470**   |

Both totals exclude the `vmaf_cuda_picture_get_pool_slot()` API addition,
which would propagate into `picture_cuda.h` (a public-ish surface).

## Why a build-and-measure prototype was not pursued

The investigation brief authorised a single-feature (PSNR) build + measure.
After working through the data-flow analysis above, the upper bound on the
gain is determinable from the existing profile **without** building the
prototype:

- Total libvmaf CPU samples in the 9344-sample profile: 4.4% of total.
- Of that, `submit_fex_cuda` self+children (excluding the
  `upload_plane_cuda` synchronous wait, which graph capture does NOT help
  because it sits in the picture pool's upload path) is approximately
  **1-2% of total samples** based on the profile children view.
- The wall-clock benchmark runs at 366 fps with three concurrent
  extractors (motion, vif, adm). At that fps the CPU thread is mostly
  idle in `cuStreamSynchronize`; reducing the submit-side libvmaf cost
  by even 50% would save ~1-2% of wall-clock at best, because the GPU
  is doing the actual frame compute in parallel with anything we
  shorten on the CPU side.
- The 10-20% prediction in the dedup profile was an **order-of-magnitude
  guess** filed under "highest potential" in the rank-by-complexity
  table, not a measured ceiling. Re-reading the relevant paragraph
  ("Stream capture / CUDA graph for the submit–readback chain (~10–20%
  potential, high complexity)") confirms the wording is speculative.

Building the prototype would burn an hour of CUDA-toolchain compile time
to confirm a ceiling we can read off the existing profile. The decision
matrix below treats "the realised gain is plausibly 1-3% wall-clock, not
10-20%" as the working hypothesis for the GO/NO-GO/DEFER call.

If a future profile (post profile-items-1-3) shows the submit chain has
become the dominant residual, the build-and-measure step should be
revisited with the API sketch above as the starting point.

## Decision matrix

| Option                                       | Realised gain         | Code cost (LOC) | Maintenance burden                  | Bit-exactness risk             | Verdict |
|----------------------------------------------|-----------------------|------------------|-------------------------------------|---------------------------------|---------|
| **A. Ship now, single feature (PSNR opt-in)**| 1-3% wall-clock (PSNR-only path) | +470 | One opt-in path; slot-array state   | Low — replay preserves submit-order | Reject  |
| **B. Ship now, all 12 CUDA features**        | 1-3% wall-clock (full pipeline)  | +1240 | High — every new feature opts in    | Low                             | Reject  |
| **C. NO-GO permanently**                     | 0%                    | 0                | Zero                                | None                            | Plausible |
| **D. DEFER until profile items 1-3 ship**    | 0% now, revisit later | 0                | Zero now; revisit on next profile   | None                            | **Recommended** |

Reject A and B because the realised gain (1-3%) is roughly half the gain
of profile item 3 (per-frame pinned-host buffer reuse, ~2-3%) and a
fraction of items 1-2 (8-12% + 5%), all of which are simpler. Spending
~470-1240 LOC to claim a 1-3% gain is a poor use of complexity budget
when ~15-20% is sitting on the table at lower complexity.

Pick D over C because:

1. The profile measurement is from a *post-dedup* baseline. Once items
   1-3 ship, the residual CPU breakdown will look different — the
   submit chain may rise in proportional importance once the upload
   path and cross-extractor sync are cheaper.
2. Newer CUDA driver releases keep improving graph-replay overhead.
   The cost-benefit can shift over a 12-month horizon.
3. The `kernel_template.h` docstring already names
   `lc->finished` as a graph-capture hook point. Leaving the door open
   in the template is free; closing it (NO-GO) would require pruning
   that comment and committing to never revisiting.

## What needs to be true for a future GO

A reasonable trigger for revisiting:

- Profile items 1-3 from `cuda-dedup-profile-2026-05-02.md` have
  shipped and a fresh profile under the same workload shows
  `submit_fex_cuda` self+children ≥ 5% of total CPU samples (roughly
  doubling its current share once the upload path and cross-extractor
  sync are no longer dominant).
- A measured single-feature (PSNR) prototype on that re-baseline
  delivers ≥ 5% wall-clock gain on the Netflix normal pair at 576x324
  with 48 frames.
- Dynamic resolution support is either out-of-scope for the workload
  or accepted as a re-capture cost.

## References

- [CUDA dedup profile 2026-05-02](../development/cuda-dedup-profile-2026-05-02.md)
  §5 "Stream capture / CUDA graph for the submit–readback chain"
- [`core/src/cuda/kernel_template.h`](../../core/src/cuda/kernel_template.h)
  — `VmafCudaKernelLifecycle.finished` documented as a graph-capture hook point
- [`core/src/feature/cuda/integer_psnr_cuda.c`](../../core/src/feature/cuda/integer_psnr_cuda.c)
  — reference consumer of the kernel template
- [`core/src/gpu_picture_pool.c`](../../core/src/gpu_picture_pool.c)
  — pool round-robin (`pic_cnt = 4` for CUDA)
- [ADR-0221 (kernel template)](../adr/0221-cuda-kernel-template.md)
- [ADR-0239 (gpu picture pool dedup)](../adr/0239-gpu-picture-pool-dedup.md)
- CUDA driver API: `cuStreamBeginCapture`, `cuStreamEndCapture`,
  `cuGraphInstantiate`, `cuGraphExecKernelNodeSetParams`, `cuGraphLaunch`
  (CUDA 13.2 toolkit, GA since CUDA 11.0)
