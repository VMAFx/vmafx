<!-- markdownlint-disable MD060 -->
# GPU per-feature kernel scaffolding templates

Status: templates introduced 2026-04-29
([ADR-0246](../adr/0246-gpu-kernel-template.md)); HIP and Metal sections added
per [ADR-0484](../adr/0484-kernel-scaffolding-hip-metal-doc.md). Active
backends: CUDA, HIP, Metal. The Vulkan backend (previously 22/22 kernels
migrated) was removed per ADR-0726. Migration coverage: **16/20 CUDA kernels**
use the template; **14 HIP kernels** and **8 Metal kernels** likewise. The four
remaining CUDA kernels (`integer_adm_cuda`, `integer_motion_cuda`,
`integer_vif_cuda`, `ssimulacra2_cuda`) use bespoke lifecycle code and remain
future migration candidates.

This page documents the **per-backend kernel scaffolding templates** that
sit alongside the CUDA, HIP, and Metal backend runtimes. (The Vulkan template
at `core/src/vulkan/kernel_template.h` was deleted with ADR-0726; its historical
design is preserved below for reference.)

- [`core/src/cuda/kernel_template.h`](../../core/src/cuda/kernel_template.h)
  — inline helpers
- [`core/src/hip/kernel_template.h`](../../core/src/hip/kernel_template.h) +
  [`core/src/hip/kernel_template.c`](../../core/src/hip/kernel_template.c) —
  out-of-line helpers backed by real ROCm HIP calls (T7-10b / ADR-0212). A
  `vmaf_hip_kernel_submit_post_record` helper covers post-dispatch
  fence-record patterns specific to HIP async recording.
- `core/src/metal/kernel_template.h` and `kernel_template.mm`
  — out-of-line helpers backed by real Metal calls (T8-1b / ADR-0420). On
  Apple Silicon unified memory the `device` / `host_pinned` split from CUDA
  collapses to a single `MTLBuffer` (`MTLResourceStorageModeShared`) whose
  `[buffer contents]` pointer is cached in `VmafMetalKernelBuffer::host_view`.

These headers absorb the lifecycle boilerplate that every fork-added GPU
feature kernel re-implements by hand. Each kernel migration is a separate
PR with its own `places=4` cross-backend gate (per
[ADR-0214](../adr/0214-gpu-parity-ci-gate.md)).

If you are writing a brand-new GPU feature kernel, prefer the templates over
copy-paste from a neighbouring kernel — the helpers wrap the steps that
historically caused regressions (forgetting `cuStreamSynchronize` before
`cuStreamDestroy`, leaking a `VkDescriptorPool` on a partial-init failure,
etc.).

## CUDA template

The CUDA template formalises the async-stream + event lifecycle every
fork-added CUDA kernel currently uses. The reference implementation is
[`integer_psnr_cuda.c`](../../core/src/feature/cuda/integer_psnr_cuda.c).

### Surface

```c
#include "cuda/kernel_template.h"

typedef struct VmafCudaKernelLifecycle {
    CUstream str;       /* private non-blocking stream for readback */
    CUevent  submit;    /* recorded post-launch on picture stream    */
    CUevent  finished;  /* recorded post-readback on str             */
} VmafCudaKernelLifecycle;

typedef struct VmafCudaKernelReadback {
    VmafCudaBuffer *device;       /* device-side accumulator     */
    void           *host_pinned;  /* pinned host readback slot   */
    size_t          bytes;
} VmafCudaKernelReadback;

int  vmaf_cuda_kernel_lifecycle_init(VmafCudaKernelLifecycle *,
                                     VmafCudaState *);

int  vmaf_cuda_kernel_readback_alloc(VmafCudaKernelReadback *,
                                     VmafCudaState *, size_t bytes);

int  vmaf_cuda_kernel_submit_pre_launch(VmafCudaKernelLifecycle *,
                                        VmafCudaState *,
                                        VmafCudaKernelReadback *,
                                        CUstream picture_stream,
                                        CUevent dist_ready_event);

int  vmaf_cuda_kernel_collect_wait(VmafCudaKernelLifecycle *,
                                   VmafCudaState *);

int  vmaf_cuda_kernel_lifecycle_close(VmafCudaKernelLifecycle *,
                                      VmafCudaState *);

int  vmaf_cuda_kernel_readback_free(VmafCudaKernelReadback *,
                                    VmafCudaState *);
```

### What each helper covers

| Helper                                  | Boilerplate it replaces                                                       |
|-----------------------------------------|-------------------------------------------------------------------------------|
| `vmaf_cuda_kernel_lifecycle_init`       | `cuCtxPushCurrent` → `cuStreamCreateWithPriority` → 2× `cuEventCreate` → pop. |
| `vmaf_cuda_kernel_readback_alloc`       | `vmaf_cuda_buffer_alloc` + `vmaf_cuda_buffer_host_alloc` pair.                |
| `vmaf_cuda_kernel_submit_pre_launch`    | `cuMemsetD8Async` zero-out + `cuStreamWaitEvent` on dist's ready event.       |
| `vmaf_cuda_kernel_collect_wait`         | `cuStreamSynchronize` on the private stream.                                  |
| `vmaf_cuda_kernel_lifecycle_close`      | Stream sync + destroy + 2× event destroy, with partial-init safety.           |
| `vmaf_cuda_kernel_readback_free`        | Device-buffer free (`vmaf_cuda_buffer_free` + `free`) **and** pinned-host free (`vmaf_cuda_buffer_host_free`). Callers do not call `vmaf_cuda_buffer_host_free` separately (PR #93 sweep, 2026-05-29). |

### What stays in the kernel TU

- The per-metric `cuLaunchKernel(...)` call (grid dims, kernel parameter pack,
  function handle).
- The `cuModuleLoadData` / `cuModuleGetFunction` chain — kernel binary names
  and symbol counts vary per metric.
- The host-side reduction and score emission. PSNR's `10 * log10(peak² / mse)`
  is one line; `ssimulacra2` has a 6-band pyramid pool. Neither belongs in a
  shared header.
- The pinned-host buffer free is now handled inside
  `vmaf_cuda_kernel_readback_free`; callers must **not** call
  `vmaf_cuda_buffer_host_free` separately on `rb->host_pinned`.
  (Pre-2026-05-29 callers that did nothing leaked the allocation;
  that bug was fixed by moving the free into the helper —
  PR #93 follow-up sweep.)

### Migration sketch

```c
typedef struct PsnrStateCuda {
    VmafCudaKernelLifecycle  lc;
    VmafCudaKernelReadback   sse;
    /* metric-specific: kernel handles, max constants, dict... */
} PsnrStateCuda;

static int init_fex_cuda(VmafFeatureExtractor *fex, ...)
{
    PsnrStateCuda *s = fex->priv;
    int err = vmaf_cuda_kernel_lifecycle_init(&s->lc, fex->cu_state);
    if (err) return err;
    err = vmaf_cuda_kernel_readback_alloc(&s->sse, fex->cu_state,
                                          sizeof(uint64_t));
    if (err) return err;
    /* metric-specific: module load + function resolve, peak constants */
    return 0;
}
```

The before/after diff for `integer_psnr_cuda.c` is roughly **−6 LOC of
host-side scaffolding** per kernel — small, but the win is mostly in the
shared error-handling and partial-init unwind paths, not the line count.

## Vulkan template (historical — backend removed in ADR-0726)

> The Vulkan backend was removed per ADR-0726 (2026-05-28). The template
> source (`core/src/vulkan/kernel_template.h`) no longer exists. The
> description below is preserved for historical context only.

The Vulkan template captured the descriptor-pool + pipeline + per-WG int64
partials shape that every Vulkan SSBO-only reduction kernel used. The
reference implementation was
[`psnr_vulkan.c`](../../core/src/feature/vulkan/psnr_vulkan.c) (deleted).

### Surface

```c
#include "vulkan/kernel_template.h"

typedef struct VmafVulkanKernelPipeline {
    VkDescriptorSetLayout dsl;
    VkPipelineLayout      pipeline_layout;
    VkShaderModule        shader;
    VkPipeline            pipeline;
    VkDescriptorPool      desc_pool;
} VmafVulkanKernelPipeline;

typedef struct VmafVulkanKernelSubmit {
    VkCommandBuffer cmd;
    VkFence         fence;
} VmafVulkanKernelSubmit;

typedef struct VmafVulkanKernelPipelineDesc {
    uint32_t                       ssbo_binding_count;
    uint32_t                       push_constant_size;
    const uint32_t                *spv_bytes;
    size_t                         spv_size;
    VkComputePipelineCreateInfo    pipeline_create_info;
    uint32_t                       max_descriptor_sets;
} VmafVulkanKernelPipelineDesc;

int  vmaf_vulkan_kernel_pipeline_create(VmafVulkanContext *,
                                        const VmafVulkanKernelPipelineDesc *,
                                        VmafVulkanKernelPipeline *);

int  vmaf_vulkan_kernel_submit_begin(VmafVulkanContext *,
                                     VmafVulkanKernelSubmit *);

int  vmaf_vulkan_kernel_submit_end_and_wait(VmafVulkanContext *,
                                            VmafVulkanKernelSubmit *);

void vmaf_vulkan_kernel_submit_free(VmafVulkanContext *,
                                    VmafVulkanKernelSubmit *);

void vmaf_vulkan_kernel_pipeline_destroy(VmafVulkanContext *,
                                         VmafVulkanKernelPipeline *);
```

### What each helper covers

| Helper                                          | Boilerplate it replaces                                                      |
|-------------------------------------------------|------------------------------------------------------------------------------|
| `vmaf_vulkan_kernel_pipeline_create`            | DSL + pipeline layout + shader module + compute pipeline + descriptor pool. |
| `vmaf_vulkan_kernel_submit_begin`               | Allocate cmd buffer + begin recording + create fence (with rollback).        |
| `vmaf_vulkan_kernel_submit_end_and_wait`        | End recording + queue submit + fence wait.                                   |
| `vmaf_vulkan_kernel_submit_free`                | Destroy fence + free cmd buffer (partial-init safe).                         |
| `vmaf_vulkan_kernel_pipeline_destroy`           | `vkDeviceWaitIdle` + reverse-order destroy of the five pipeline objects.    |

### What stays in the kernel TU

- The shader bytecode header (`<feature>_spv.h`) — generated per-kernel by
  the `subdir('vulkan')` glslc chain.
- The push-constant struct layout. `PsnrPushConsts` and a hypothetical
  `Ssim4VifPushConsts` have nothing in common.
- Spec-constant population — the caller fills
  `pipeline_create_info.stage.pSpecializationInfo` before calling
  `vmaf_vulkan_kernel_pipeline_create`.
- Per-frame buffer alloc, host upload, descriptor-set allocation +
  binding-write, dispatch grid math, host-side reduction. These shapes
  diverge enough between kernels that a unified API would be either
  too narrow (just PSNR's shape) or too generic (callbacks for
  everything).

### Migration sketch

```c
typedef struct PsnrVulkanState {
    VmafVulkanContext        *ctx;
    int                       owns_ctx;
    VmafVulkanKernelPipeline  pl;
    /* metric-specific: per-plane buffers, push-const cache, ... */
} PsnrVulkanState;

static int init(VmafFeatureExtractor *fex, ...)
{
    PsnrVulkanState *s = fex->priv;
    /* ... resolve s->ctx ... */
    VmafVulkanKernelPipelineDesc desc = {
        .ssbo_binding_count = 3,
        .push_constant_size = sizeof(PsnrPushConsts),
        .spv_bytes          = psnr_spv,
        .spv_size           = psnr_spv_size,
        .max_descriptor_sets = 12,
        /* caller fills stage.pName + spec_info on pipeline_create_info */
    };
    desc.pipeline_create_info.stage.pName = "main";
    desc.pipeline_create_info.stage.pSpecializationInfo = &spec_info;
    return vmaf_vulkan_kernel_pipeline_create(s->ctx, &desc, &s->pl);
}

static int extract(VmafFeatureExtractor *fex, ...)
{
    PsnrVulkanState *s = fex->priv;
    VmafVulkanKernelSubmit sub;
    int err = vmaf_vulkan_kernel_submit_begin(s->ctx, &sub);
    if (err) return err;

    /* metric-specific: allocate descriptor sets, write bindings,
     * record commands on sub.cmd, etc. */

    err = vmaf_vulkan_kernel_submit_end_and_wait(s->ctx, &sub);
    /* host-side reduce + score emit */
    vmaf_vulkan_kernel_submit_free(s->ctx, &sub);
    return err;
}

static int close_fex(VmafFeatureExtractor *fex)
{
    PsnrVulkanState *s = fex->priv;
    vmaf_vulkan_kernel_pipeline_destroy(s->ctx, &s->pl);
    /* metric-specific frees */
    return 0;
}
```

The before/after diff for `psnr_vulkan.c` is roughly **−30 LOC** — the
five vkCreate/vkDestroy pairs collapse into two helper calls each, and
the cleanup `goto`-ladder loses two labels.

## HIP template

The HIP template mirrors the CUDA template field-for-field (T7-10b /
[ADR-0212](../adr/0212-hip-backend-scaffold.md)). Unlike the CUDA variant
which uses `static inline` helpers, the HIP helpers are out-of-line
(`kernel_template.c`) because the ROCm HIP driver-loader table was not
scaffolded at the time and the bodies require a `-ENOSYS` guard that
inline callers cannot override. The runtime PR (T7-10b) replaced the
stub bodies with real `hipStreamCreate` / `hipEventCreate` /
`hipMemcpyAsync` calls. The struct shapes and helper signatures are stable.

### Surface differences from CUDA

| Aspect | CUDA | HIP |
|--------|------|-----|
| Stream handle type | `CUstream` (via `CudaFunctions` table) | `hipStream_t` stored as `uintptr_t` in struct |
| Event handle type | `CUevent` | `hipEvent_t` stored as `uintptr_t` |
| Helper linkage | `static inline` in `.h` | Out-of-line in `kernel_template.c` |
| Extra helper | — | `vmaf_hip_kernel_submit_post_record` (post-dispatch fence record) |

### What each helper covers

| Helper | Boilerplate it replaces |
|--------|------------------------|
| `vmaf_hip_kernel_lifecycle_init` | `hipStreamCreateWithFlags` + 2x `hipEventCreateWithFlags`. |
| `vmaf_hip_kernel_readback_alloc` | `hipMallocAsync` + `hipHostMalloc` pair. |
| `vmaf_hip_kernel_submit_pre_launch` | Device-accumulator zero + `hipStreamWaitEvent` on dist ready. |
| `vmaf_hip_kernel_collect_wait` | `hipStreamSynchronize` on the private stream. |
| `vmaf_hip_kernel_lifecycle_close` | Stream sync + destroy + 2x event destroy with partial-init safety. |
| `vmaf_hip_kernel_readback_free` | `hipFree` (device) + `hipHostFree` (pinned host). |
| `vmaf_hip_kernel_submit_post_record` | Post-dispatch `hipEventRecord` on `lc->submit`. |

## Metal template

The Metal template mirrors the HIP template with one unified-memory
simplification (T8-1b / [ADR-0420](../adr/0420-metal-backend-runtime-t8-1b.md)):
on Apple Silicon the `device` / `host_pinned` pair collapses to a single
`MTLBuffer` allocated with `MTLResourceStorageModeShared`. Helpers are
out-of-line Objective-C++ in `kernel_template.mm` (ARC) and bridge
`uintptr_t` slots to `id<MTL...>` via `__bridge_retained` /
`__bridge_transfer`.

### Surface differences from HIP

| Aspect | HIP | Metal |
|--------|-----|-------|
| Stream/queue type | `hipStream_t` | `MTLCommandQueue` (as `uintptr_t`) |
| Buffer split | `device` + `host_pinned` | Single `MTLBuffer` + `host_view` pointer |
| Memory model | Discrete PCIe (AMD dGPU) | Unified DRAM (Apple Silicon) |
| Kernel language | HIP C++ / HSACO | Metal Shading Language → `.metallib` |
| Helper TU language | C | Objective-C++ (`.mm`, ARC) |

### What each helper covers

| Helper | Boilerplate it replaces |
|--------|------------------------|
| `vmaf_metal_kernel_lifecycle_init` | `[device newCommandQueue]` + 2x `[device newSharedEvent]`. |
| `vmaf_metal_kernel_buffer_alloc` | `[device newBufferWithLength:options:MTLResourceStorageModeShared]`. |
| `vmaf_metal_kernel_submit_pre_launch` | Blit-fill zero + `[cmd addCompletedHandler:]` fence setup. |
| `vmaf_metal_kernel_collect_wait` | `[commandBuffer waitUntilCompleted]`. |
| `vmaf_metal_kernel_lifecycle_close` | Command-queue drain + shared-event release (partial-init safe). |
| `vmaf_metal_kernel_buffer_free` | `__bridge_transfer` release of the `MTLBuffer`. |

## Lifecycle contract (shared across all four backends)

Every backend follows the same four-phase sequence:

1. **Init** — allocate stream/queue, events, device accumulator, host
   readback slot. Returns `0` or a negative errno. Partial failures
   roll back in reverse order.
2. **Submit** — zero the accumulator, wait on the dist-ready event,
   launch the kernel, record a completion event.
3. **Collect** — wait for the completion event on the private
   stream/queue; copy the result to the host readback slot.
4. **Close** — synchronise the stream/queue, destroy events, free
   device and host buffers in reverse allocation order.

Backends diverge in how they represent handles (`CUstream` vs
`hipStream_t` vs `MTLCommandQueue`) and in whether SSBO descriptors
(Vulkan) or device/host buffer splits (CUDA/HIP) apply. The
four-phase contract itself is invariant.

## Migrating an existing kernel

Each kernel migration is its own PR, gated by:

1. **Netflix golden** (CPU only, untouched — the kernel doesn't run on the
   CPU lane).
2. **`/cross-backend-diff`** at `places=4` against the CPU reference, on
   every Netflix golden YUV pair the kernel is registered against (per
   [ADR-0214](../adr/0214-gpu-parity-ci-gate.md)).
3. The repo's standard `make lint` clean on every touched file
   (per [CLAUDE.md §12 r12](../../CLAUDE.md)).

Migration status and remaining candidates:

| Backend | Kernel | Status |
|---------|--------|--------|
| CUDA | 16/20 kernels | Migrated |
| CUDA | `integer_adm_cuda` | Remaining (bespoke multi-stream) |
| CUDA | `integer_motion_cuda` | Remaining (ping-pong blur ring) |
| CUDA | `integer_vif_cuda` | Remaining (multi-scale dispatch) |
| CUDA | `ssimulacra2_cuda` | Remaining (multi-readback pyramid) |
| Vulkan | 22/22 kernels | Fully migrated |
| HIP | 14 kernels | Migrated via T7-10b sweep |
| Metal | 8 kernels | Migrated via T8-1b/T8-1c sweep |

Migrations are tracked as `T7-XX-followup-{a,b,c}` in `CHANGELOG.md`.

## Why per-backend (not cross-backend)

Sister-agent's GPU-template scope analysis (referenced by ADR-0246)
established that CUDA's async-stream + event model and Vulkan's
command-buffer + fence + descriptor-pool model share **no concrete
shape**. A cross-backend abstraction would force a lowest-common-denominator
API that captures neither well. The per-backend split keeps each header
honest about the platform it targets.

## Why helper functions (not macros)

CUDA and Vulkan templates use `static inline` helpers; HIP and Metal use
out-of-line helpers for reasons described in their sections above. Shared
trade-offs for all four backends:

- **Debug stepping**: `cuda-gdb` / Nsight / RenderDoc / vkconfig step through
  inline functions; macros expand to a single compound statement that
  shows up as one line in the source view.
- **Type-checking**: missing parameters or wrong-type pointers produce
  compiler errors at the helper site, not at some inscrutable point inside
  a macro expansion.
- **The macros that do pay off live elsewhere**:
  [`cuda_helper.cuh`](../../core/src/cuda/cuda_helper.cuh) provides
  `CHECK_CUDA_GOTO` / `CHECK_CUDA_RETURN`, which are macros precisely because
  their `goto label` form needs textual substitution. The kernel-template
  helpers use those macros internally.

## See also

- [ADR-0246](../adr/0246-gpu-kernel-template.md) — original CUDA + Vulkan
  template design decision and alternatives.
- [ADR-0484](../adr/0484-kernel-scaffolding-hip-metal-doc.md) — HIP and
  Metal section addition.
- [`core/src/cuda/AGENTS.md`](../../core/src/cuda/AGENTS.md) — kernel
  template invariant row.
- [`core/src/vulkan/AGENTS.md`](../../core/src/vulkan/AGENTS.md) —
  kernel template invariant row.
- [`core/src/hip/AGENTS.md`](../../core/src/hip/AGENTS.md) — HIP
  kernel template invariant row.
- [`core/src/metal/AGENTS.md`](../../core/src/metal/AGENTS.md) — Metal
  kernel template invariant row.
- [`docs/backends/cuda/overview.md`](cuda/overview.md) — broader CUDA
  backend overview.
- [`docs/backends/vulkan/overview.md`](vulkan/overview.md) — broader Vulkan
  backend overview.
- [`docs/backends/hip/index.md`](hip/index.md) — broader HIP backend
  overview.
- [`docs/backends/metal/index.md`](metal/index.md) — broader Metal backend
  overview.
