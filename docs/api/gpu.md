<!-- markdownlint-disable MD013 -->
# GPU backends C API — `libvmaf_cuda.h` / `libvmaf_sycl.h` / `libvmaf_hip.h` / `libvmaf_metal.h`

Each GPU backend adds its own small API on top of the core
`libvmaf.h` surface — a state object, picture preallocation helpers, and
(SYCL / Metal) zero-copy import paths. This page is the reference for
the four active backends (CUDA, SYCL, HIP, Metal); the Vulkan backend was
removed in [ADR-0726](../adr/0726-drop-vulkan-backend.md) and only a compact
removal notice remains below. HIP still has
three unported feature kernels, while Metal has a live Apple-Silicon runtime
and first kernel batch.

Core API primer: [index.md](index.md). CLI equivalents:
[../usage/cli.md#backend-selection](../usage/cli.md#backend-selection).
Backend dispatch rules + runtime precedence:
[../backends/index.md](../backends/index.md).

## When these headers apply

- The CUDA header is only useful in a build with `-Denable_cuda=true`
  (linking CUDA runtime + nvcc-compiled kernels). When disabled, the symbols
  are absent and calls won't link.
- The SYCL header is only useful in a build with `-Denable_sycl=true`
  (linking oneAPI / Level Zero). Same rule.
- The HIP header requires `-Denable_hip=true -Denable_hipcc=true` (linking
  ROCm). All feature kernels are real; 3 legacy stubs (`adm_hip`, `vif_hip`,
  `motion_hip`) return `-ENOSYS` (older `_init/_run/_destroy` API, not
  registered extractors).
- The Metal header requires `-Denable_metal=auto/enabled` on macOS.
  Runtime entry points are live on Apple Silicon; unsupported devices
  return `-ENODEV`. Eight feature kernels are currently wired.
- To write portable code that compiles against any libvmaf build, wrap
  GPU-specific sections in `#ifdef HAVE_CUDA` / `#ifdef HAVE_SYCL` /
  `#ifdef HAVE_HIP` / `#ifdef HAVE_METAL`, which
  `pkg-config --cflags libvmaf` surfaces automatically.
  (`HAVE_VULKAN` is no longer defined — Vulkan was removed in ADR-0726.)

## CUDA

### Header

[`core/include/libvmaf/libvmaf_cuda.h`](../../core/include/libvmaf/libvmaf_cuda.h)

### Lifecycle addition

```text
  vmaf_init()
  vmaf_cuda_state_init()         ← new
  vmaf_cuda_import_state()       ← new; hands state to ctx
  vmaf_cuda_preallocate_pictures()
  loop:
    vmaf_cuda_fetch_preallocated_picture()
    ...                                     write into .data[i]
    vmaf_read_pictures()
  vmaf_score_pooled()
  vmaf_close()              /* only 0 destroys the copy; nonzero must be retried */
  vmaf_cuda_state_free()    /* always required — frees the original allocation */
```

### State

```c
typedef struct VmafCudaState VmafCudaState;

typedef struct VmafCudaConfiguration {
    void *cu_ctx;   /* CUcontext; NULL → libvmaf creates one on device 0 */
} VmafCudaConfiguration;

int vmaf_cuda_state_init(VmafCudaState **cu_state, VmafCudaConfiguration cfg);
int vmaf_cuda_import_state(VmafContext *vmaf, VmafCudaState *cu_state);
int vmaf_cuda_state_free(VmafCudaState *cu_state);
```

- `cu_ctx = NULL` — libvmaf creates a fresh CUDA context on CUDA device 0.
  This is the common case for standalone tooling.
- `cu_ctx != NULL` — must be a `CUcontext` from the driver API; libvmaf
  adopts it for all allocations. Use this to interop with an application
  that already owns a context (e.g. NVENC / NVDEC).

### Ownership and explicit free

`vmaf_cuda_import_state(vmaf, cu_state)` copies the `VmafCudaState`
**by value** into the `VmafContext` — it does not transfer ownership of
the original heap allocation. A successful `vmaf_close(vmaf)` tears down the
**embedded copy** (destroying the CUDA stream and, if libvmaf created the
context, releasing the primary context). Any nonzero close result retains the
`VmafContext` and any CUDA handle whose driver release failed. Keep the
original `cu_state` and every other imported dependency alive and retry
`vmaf_close(vmaf)`. Only after close returns 0 may the caller invoke
`vmaf_cuda_state_free(cu_state)` to release the original heap allocation.
Skipping that final call leaks the allocation. A state may be imported into
exactly one context, and a context's live CUDA state cannot be overwritten;
duplicate import returns `-EBUSY`.

`vmaf_cuda_state_free(VmafCudaState *cu_state)` (added in
[ADR-0157](../adr/0157-cuda-preallocation-leak-netflix-1300.md))
is a NULL-safe `free()` wrapper for the original pointer returned by
`vmaf_cuda_state_init()`. At the time of the call, `vmaf_close()` has
already run `vmaf_cuda_release()` on the embedded copy (destroying the
stream and context), so `vmaf_cuda_state_free()` only needs to `free()`
the struct. It also serves as the escape hatch when the state was built
via `vmaf_cuda_state_init()` but never imported (e.g. an early setup failure),
in which case it additionally tears down the stream and context before
freeing. That teardown is retry-safe: a nonzero result retains the state and
its live handles for another `vmaf_cuda_state_free()` call.

```c
VmafCudaState *cuda = NULL;
int err = vmaf_cuda_state_init(&cuda, (VmafCudaConfiguration){ .cu_ctx = NULL });
if (err) { return err; }

if (some_unrelated_setup_failed()) {
    vmaf_cuda_state_free(cuda);   /* not yet imported — also destroys stream/ctx */
    return -1;
}

err = vmaf_cuda_import_state(ctx, cuda);
/* ctx now holds a by-value copy of the state.
 * A successful vmaf_close(ctx) destroys that copy (stream + context).
 * vmaf_cuda_state_free(cuda) must still be called after success to
 * release the original heap allocation from vmaf_cuda_state_init(). */
int close_err = vmaf_close(ctx);
if (close_err != 0)
    close_err = vmaf_close(ctx); /* retained teardown-only context */
if (close_err == 0)
    vmaf_cuda_state_free(cuda);  /* original allocation outlives close */
```

The CUDA and SYCL lifetime models differ deliberately: CUDA state is
copied by value into the context; the caller still owns the heap pointer.
SYCL state is also always caller-freed after `vmaf_close()` returns 0 (the queue is
queue-scoped and survives a scoring session boundary). Both require an
explicit free only after exact-zero close — CUDA via `vmaf_cuda_state_free`,
SYCL via `vmaf_sycl_state_free`. Match the API to the lifetime model of
the underlying runtime.

### Picture preallocation

```c
enum VmafCudaPicturePreallocationMethod {
    VMAF_CUDA_PICTURE_PREALLOCATION_METHOD_NONE,
    VMAF_CUDA_PICTURE_PREALLOCATION_METHOD_DEVICE,
    VMAF_CUDA_PICTURE_PREALLOCATION_METHOD_HOST,
    VMAF_CUDA_PICTURE_PREALLOCATION_METHOD_HOST_PINNED,
};

typedef struct VmafCudaPictureConfiguration {
    struct { unsigned w, h; unsigned bpc; enum VmafPixelFormat pix_fmt; } pic_params;
    enum VmafCudaPicturePreallocationMethod pic_prealloc_method;
} VmafCudaPictureConfiguration;

int vmaf_cuda_preallocate_pictures(VmafContext *ctx, VmafCudaPictureConfiguration cfg);
int vmaf_cuda_fetch_preallocated_picture(VmafContext *ctx, VmafPicture *pic);
```

Preallocation methods:

| Method | `.data[i]` memory | Use when |
| --- | --- | --- |
| `NONE` | caller-provided | You already own device buffers and set `.data[i]` yourself before `vmaf_read_pictures()`. |
| `DEVICE` | `cudaMalloc` | Your source data already lives on GPU (decoder output, encoder input). No H2D copy on `vmaf_read_pictures`. |
| `HOST` | `malloc` | Your source is on host; libvmaf inserts the H2D copy. |
| `HOST_PINNED` | `cudaMallocHost` | Host source but you want overlap with async compute — pinned memory allows concurrent DMA. |

`HOST_PINNED` is almost always the right choice for CPU-decoded feeds; the
peak throughput difference vs `HOST` on a PCIe-4 x16 link is 15–25% for
1080p. See [../backends/cuda/overview.md](../backends/cuda/overview.md).

### Complete CUDA example

```c
#include <cuda.h>
#include <libvmaf/libvmaf.h>
#include <libvmaf/libvmaf_cuda.h>
#include <libvmaf/model.h>
#include <libvmaf/picture.h>

int main(void) {
    VmafConfiguration cfg = { .log_level = VMAF_LOG_LEVEL_WARNING, .n_threads = 4 };
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);

    VmafCudaState *cuda = NULL;
    VmafCudaConfiguration cu_cfg = { .cu_ctx = NULL };  /* libvmaf picks device 0 */
    err = vmaf_cuda_state_init(&cuda, cu_cfg);
    err = vmaf_cuda_import_state(vmaf, cuda);

    VmafModel *model = NULL;
    VmafModelConfig mcfg = { .name = "vmaf" };
    err = vmaf_model_load(&model, &mcfg, "vmaf_v0.6.1");
    err = vmaf_use_features_from_model(vmaf, model);

    VmafCudaPictureConfiguration pcfg = {
        .pic_params = { .w = 1920, .h = 1080, .bpc = 8, .pix_fmt = VMAF_PIX_FMT_YUV420P },
        .pic_prealloc_method = VMAF_CUDA_PICTURE_PREALLOCATION_METHOD_HOST_PINNED,
    };
    err = vmaf_cuda_preallocate_pictures(vmaf, pcfg);

    for (unsigned i = 0; i < nframes; i++) {
        VmafPicture ref = {0}, dist = {0};
        err = vmaf_cuda_fetch_preallocated_picture(vmaf, &ref);
        err = vmaf_cuda_fetch_preallocated_picture(vmaf, &dist);
        /* fill ref.data[i] / dist.data[i] — host-pinned, write normally */
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
    }

    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);

    double score;
    err = vmaf_score_pooled(vmaf, model, VMAF_POOL_METHOD_MEAN, &score, 0, UINT_MAX);
    printf("VMAF: %.17g\n", score);

    int close_err = vmaf_close(vmaf);
    if (close_err != 0)
        close_err = vmaf_close(vmaf); /* retained teardown-only context */
    if (close_err == 0) {
        vmaf = NULL;
        vmaf_cuda_state_free(cuda);
        vmaf_model_destroy(model);
    } else {
        return 1; /* retain dependencies through fail-closed process exit */
    }
    return 0;
}
```

### Limitations

- Single device. `VmafCudaConfiguration` does not expose a device index;
  launch libvmaf on device N by setting the current context to N before
  `vmaf_cuda_state_init()` (via `cuCtxSetCurrent` or `cudaSetDevice`).
- No stream parameter. libvmaf runs its own streams internally; interop with
  an external stream is not exposed in v1.

## SYCL

### Header

[`core/include/libvmaf/libvmaf_sycl.h`](../../core/include/libvmaf/libvmaf_sycl.h)

### State

```c
typedef struct VmafSyclState VmafSyclState;

typedef struct VmafSyclConfiguration {
    int device_index;        /* -1 = SYCL default device; 0+ = specific ordinal */
    int enable_profiling;    /* non-zero: queue w/ enable_profiling property */
} VmafSyclConfiguration;

int  vmaf_sycl_state_init(VmafSyclState **out, VmafSyclConfiguration cfg);
int  vmaf_sycl_import_state(VmafContext *ctx, VmafSyclState *state);
void vmaf_sycl_state_free(VmafSyclState **state);
int  vmaf_sycl_list_devices(void);
```

`vmaf_sycl_state_free` is unusual — SYCL state is *not* owned by the context
after import. You must call it explicitly only after `vmaf_close(ctx)` returns
0; any nonzero close retains the state dependency for retry. This asymmetry
exists because SYCL USM allocations are queue-scoped and the queue outlives one
scoring session.

`vmaf_sycl_list_devices` enumerates `device_type::gpu` only (CPU / FPGA /
accelerator devices are skipped) and prints one line per device with its
ordinal, platform, vendor, driver version, and fp64 support flag. Returns
the count, or `-EIO` on a SYCL exception. Used by `vmaf_bench --list-devices`
([../usage/bench.md](../usage/bench.md)).

### Picture preallocation (simple path)

```c
enum VmafSyclPicturePreallocationMethod {
    VMAF_SYCL_PICTURE_PREALLOCATION_METHOD_NONE = 0,
    VMAF_SYCL_PICTURE_PREALLOCATION_METHOD_DEVICE = 1,
    VMAF_SYCL_PICTURE_PREALLOCATION_METHOD_HOST = 2,
};

typedef struct VmafSyclPictureConfiguration {
    struct { unsigned w, h; unsigned bpc; enum VmafPixelFormat pix_fmt; } pic_params;
    enum VmafSyclPicturePreallocationMethod pic_prealloc_method;
} VmafSyclPictureConfiguration;

int vmaf_sycl_preallocate_pictures(VmafContext *ctx, VmafSyclPictureConfiguration cfg);
int vmaf_sycl_picture_fetch(VmafContext *ctx, VmafPicture *pic);
```

`vmaf_sycl_preallocate_pictures` now honors the enum and creates a 2-deep
SYCL picture pool when `DEVICE` or `HOST` is selected:

These numeric values are stable and append-only; serialized configuration and
FFI bindings may rely on them.

| Method | Backing | Use case |
| --- | --- | --- |
| `VMAF_SYCL_PICTURE_PREALLOCATION_METHOD_NONE` | no pool; `vmaf_sycl_picture_fetch` falls back to `vmaf_picture_alloc()` | CPU-fed callers and test harnesses |
| `VMAF_SYCL_PICTURE_PREALLOCATION_METHOD_DEVICE` | `sycl::malloc_device` USM | decoder / uploader writes directly into GPU-resident planes |
| `VMAF_SYCL_PICTURE_PREALLOCATION_METHOD_HOST` | `sycl::malloc_host` USM | CPU-visible pooled planes with SYCL-friendly lifetime semantics |

The caller owns each `VmafPicture` reference returned by
`vmaf_sycl_picture_fetch()` and must release it with `vmaf_picture_unref()`
after submitting it through `vmaf_read_pictures()`. The pool keeps its own
references until `vmaf_close()` returns 0 and tears down the context.
If close returns nonzero, the context and its pool ownership remain retained
for retry; only exact zero commits teardown.

### Zero-copy frame-buffer path

For callers that own a GPU-resident decode pipeline (Intel VPL, VA-API,
D3D11), the frame-buffer API exposes two shared Y-plane buffers (ref + dis)
and alternative ingest entry points. Use it when the pipeline wants the
SYCL backend's built-in double-buffered Y-plane upload path instead of
managing whole `VmafPicture` instances from the preallocation pool.

```c
int vmaf_sycl_init_frame_buffers (VmafContext *ctx, unsigned w, unsigned h, unsigned bpc);
int vmaf_sycl_get_frame_buffers  (VmafContext *ctx, void **ref, void **dis);
int vmaf_read_pictures_sycl      (VmafContext *ctx, unsigned index);  /* replaces vmaf_read_pictures */
int vmaf_sycl_wait_compute       (VmafContext *ctx);
int vmaf_flush_sycl              (VmafContext *ctx);                  /* replaces (NULL,NULL,0) flush */
```

Typical zero-copy loop:

```c
vmaf_sycl_init_frame_buffers(vmaf, W, H, 8);
void *ref_buf = NULL, *dis_buf = NULL;
vmaf_sycl_get_frame_buffers(vmaf, &ref_buf, &dis_buf);

for (unsigned i = 0; i < nframes; i++) {
    /* write Y-plane luma directly into ref_buf / dis_buf
     * (kernels, SYCL events, dmabuf imports — whatever the decoder exposes) */
    vmaf_read_pictures_sycl(vmaf, i);
    /* vmaf_sycl_wait_compute() is only needed if you must reuse ref_buf/dis_buf
     * for a later frame while compute is still in flight */
}
vmaf_flush_sycl(vmaf);
```

### GPU-resident import paths

```c
/* Linux / Level Zero */
int  vmaf_sycl_dmabuf_import (VmafSyclState *state, int fd, size_t size, void **ptr);
void vmaf_sycl_dmabuf_free   (VmafSyclState *state, void *ptr);

int  vmaf_sycl_import_va_surface (VmafSyclState *state, void *va_display,
                                  unsigned int va_surface, int is_ref,
                                  unsigned w, unsigned h, unsigned bpc);

int  vmaf_sycl_upload_plane (VmafSyclState *state, const void *src, unsigned pitch,
                             int is_ref, unsigned w, unsigned h, unsigned bpc);

/* Windows (conditional) */
#ifdef _WIN32
int  vmaf_sycl_import_d3d11_surface (VmafSyclState *state, void *d3d11_device,
                                     void *d3d11_texture, unsigned subresource,
                                     int is_ref, unsigned w, unsigned h, unsigned bpc);
#endif
```

- `vmaf_sycl_dmabuf_import` is the **primitive** — turns a DMA-BUF fd into a
  SYCL device pointer via Level Zero external memory import. Stable.
- `vmaf_sycl_import_va_surface` is the **convenience wrapper** on top of
  dmabuf — preferred path for a VA-API decode feed. Falls back to
  `vaGetImage + memcpy` when the DRM-PRIME export fails (older Mesa /
  proprietary drivers).
- `vmaf_sycl_upload_plane` is the **platform-agnostic escape hatch** —
  `memcpy` from a host pointer. Use when nothing better works or when you
  need a baseline for benchmarking.
- `vmaf_sycl_import_d3d11_surface` (Windows only) is **declared in the
  public header but not implemented in-tree** (`rg` finds zero
  definitions). The Doxygen block describes an intended host-roundtrip
  design — staging texture → CPU map → H2D memcpy — but no translation
  unit provides the symbol today. Tracked as
  [issue #27](https://github.com/VMAFx/vmafx/issues/27). On Windows,
  use `vmaf_sycl_upload_plane` for a host → USM fallback.

See [ADR-0016](../adr/0016-sycl-to-master-merge-conflict-policy.md) for how
these APIs landed and [../backends/sycl/overview.md](../backends/sycl/overview.md)
for the ingestion-path decision tree.

### Profiling helpers

```c
int  vmaf_sycl_profiling_enable     (VmafSyclState *state);
void vmaf_sycl_profiling_disable    (VmafSyclState *state);
void vmaf_sycl_profiling_print      (VmafSyclState *state);
int  vmaf_sycl_profiling_get_string (VmafSyclState *state, char **out);
```

Profiling must be enabled *at init time* — the SYCL queue is created with
the `enable_profiling` property inside `vmaf_sycl_state_init()` only when
`VmafSyclConfiguration.enable_profiling = 1` is passed. `vmaf_sycl_profiling_enable`
does **not** re-create the queue; it only flips a `bool` on the state
(`core/src/sycl/common.cpp:1053`). If the queue was not built with
`enable_profiling`, calling `vmaf_sycl_profiling_enable` succeeds but
subsequent `get_profiling_info` calls on kernel events will throw a
`sycl::exception`. In practice: set `enable_profiling=1` at init, then use
the enable/disable pair to gate which frame ranges get timed.

`vmaf_sycl_profiling_get_string` yields a caller-owned buffer — free with
`free()`. Equivalent to `vmaf_bench --gpu-profile`
([../usage/bench.md](../usage/bench.md#performance-benchmark-default)).

### Limitations

- Zero-copy ingest paths (`dmabuf_import`, `import_va_surface`) require
  the SYCL queue to use the Level Zero backend — they call
  `sycl::get_native<ext_oneapi_level_zero>` directly. On an OpenCL-backend
  SYCL build these throw `sycl::exception`, which the wrapper catches and
  converts to `-EIO`. The error log is generic (`"SYCL DMA-BUF import
  exception: <what()>"`) rather than a specific "not on Level Zero"
  diagnostic, so callers that want a graceful degradation should detect
  their own backend via `sycl::queue::get_backend()` up front and fall
  back to `vmaf_sycl_upload_plane` without relying on the log text.
- `vmaf_sycl_import_d3d11_surface` is **declared but unimplemented**
  (ghost symbol — see [issue #27](https://github.com/VMAFx/vmafx/issues/27)).
  Windows callers must use `vmaf_sycl_upload_plane` today.
- `vmaf_sycl_init_frame_buffers` is single-resolution. Changing `w`/`h`/`bpc`
  mid-stream requires `vmaf_close` + re-init.

## Vulkan (removed)

Vulkan support, its public header, CLI options, feature kernels, and FFmpeg
integration were removed by [ADR-0726](../adr/0726-drop-vulkan-backend.md).
No Vulkan entry point exists in current builds; lingering Vulkan API examples
are stale. Use [CUDA](#cuda), [SYCL](#sycl), [HIP](#hip), or [Metal](#metal).

## HIP

### Header

[`core/include/libvmaf/libvmaf_hip.h`](../../core/include/libvmaf/libvmaf_hip.h)

`libvmaf_hip.h` exposes the AMD ROCm/HIP lifecycle surface. It is available
only in builds with `-Denable_hip=true -Denable_hipcc=true`; without those
flags the symbols are absent and calls will not link. HIP runtime types
(`hipDevice_t`, `hipStream_t`) cross the public ABI as `uintptr_t` to keep
the header free of `<hip/hip_runtime.h>` — cast on the caller side.

### Core lifecycle API

| Symbol | Description |
| --- | --- |
| `vmaf_hip_available` | Returns 1 if libvmaf was built with `-Denable_hip=true`, 0 otherwise. Cheap to call; no HIP runtime is touched until `vmaf_hip_state_init()`. |
| `vmaf_hip_state_init` | Allocates a `VmafHipState` pinned to a HIP device. `device_index = -1` selects the first compute-capable HIP device; 0+ selects a specific ordinal. Returns `-ENODEV` when no compatible device is found. |
| `vmaf_hip_import_state` | Hands an allocated `VmafHipState` to a `VmafContext`. The caller retains ownership and must call `vmaf_hip_state_free` only after `vmaf_close` returns 0. Returns `0` on success, `-EINVAL` when `ctx` or `state` is `NULL`, `-ENOSYS` when built without HIP. |
| `vmaf_hip_state_free` | Releases a state allocated via `vmaf_hip_state_init`. Safe to pass `NULL` or a state that was never imported. Sets the pointer to `NULL` on return. |
| `vmaf_hip_list_devices` | Enumerates compute-capable HIP devices visible to the runtime. Prints one line per device with its ordinal, name, and compute capability. Returns device count or `-ENOSYS` when built without HIP. |

### State

```c
typedef struct VmafHipState VmafHipState;

typedef struct VmafHipConfiguration {
    int device_index; /**< -1 = first HIP device with compute capability */
    int flags;        /**< reserved for future use; pass 0 */
} VmafHipConfiguration;

int  vmaf_hip_available(void);
int  vmaf_hip_state_init(VmafHipState **out, VmafHipConfiguration cfg);
int  vmaf_hip_import_state(VmafContext *ctx, VmafHipState *state);
void vmaf_hip_state_free(VmafHipState **state);
int  vmaf_hip_list_devices(void);
```

### Ownership

The HIP backend follows the same caller-owned-state model as SYCL: after
`vmaf_hip_import_state(ctx, state)` the **caller still owns the state** and
must call `vmaf_hip_state_free(&state)` only after `vmaf_close(ctx)` returns 0.
Any nonzero close retains both the context and its borrowed state dependency.
Unlike CUDA's by-value embedded copy, HIP keeps a borrowed pointer; in both
cases the caller still frees the original state allocation after successful
close. HIP state may outlive a single scoring session when the caller manages
a multi-pass workflow against the same device.

### Typical call sequence

```text
vmaf_init()
vmaf_hip_state_init(&state, cfg)     ← allocate state for device N
vmaf_hip_import_state(vmaf, state)   ← hands state to ctx; caller still owns it
loop:
  vmaf_read_pictures(vmaf, &ref, &dist, i)
vmaf_score_pooled(vmaf, ...)
vmaf_close(vmaf)                      ← retry on nonzero; only 0 invalidates
vmaf_hip_state_free(&state)          ← caller frees only after exact 0
```

### Limitations and current feature coverage

The HIP backend is compile-time gated behind `-Denable_hip=true` and requires
ROCm 7.0+ at runtime. As of ADR-0533 / ADR-0539, 21 feature extractors are
registered and end-to-end verified on AMD gfx hardware:

PSNR, float-PSNR, CIEDE, float-moment, integer-moment, float-SSIM,
MS-SSIM, PSNR-HVS, CAMBI, SSIMULACRA2, integer-motion, integer-motion-v2,
float-motion, float-VIF, integer-VIF, integer-ADM, float-ADM,
integer-SSIM, speed-chroma, speed-temporal, integer-CIEDE.

Three legacy-API stubs (`adm_hip`, `vif_hip`, `motion_hip`) exist in tree but
use an older `_init/_run/_destroy` API shape that is not compatible with the
`VmafFeatureExtractor` registration system; they return `-ENOSYS` at `init()`
and are not selectable via `--feature`. The `float_ansnr` extractor was
removed in commit 70ed8b3ce3 (PR #38); it is no longer registered on any
backend.

See [../backends/hip/overview.md](../backends/hip/overview.md) for the
complete extractor table, build flags, HSACO fat-binary target selection,
FFmpeg integration (`hip_device=N` — ADR-0380), and per-kernel notes.

## Metal

`libvmaf_metal.h` exposes the Apple Metal lifecycle and IOSurface import
surface. It is available only on macOS builds with `-Denable_metal=auto` or
`-Denable_metal=enabled`; unsupported hosts return `-ENODEV` instead of
silently falling back to CPU. The header is installed into the system prefix by
`meson install` whenever Metal is enabled, so that downstream FFmpeg
`--enable-libvmaf-metal` configure probes can locate it (ADR-0437).

### Core lifecycle API

| Symbol | Description |
| --- | --- |
| `vmaf_metal_available` | Returns 1 if the library was built with Metal support; 0 otherwise. |
| `vmaf_metal_state_init` | Allocates a `VmafMetalState`, selecting a device by index (-1 = system default). Returns `-ENODEV` on non-Apple-Family-7 hosts. |
| `vmaf_metal_import_state` | Hands an allocated `VmafMetalState` to a `VmafContext` for use during feature extraction. The caller retains ownership and must call `vmaf_metal_state_free` only after `vmaf_close` returns 0. |
| `vmaf_metal_state_free` | Releases a state allocated via `vmaf_metal_state_init` or `vmaf_metal_state_init_external`. Safe to pass `NULL`. |
| `vmaf_metal_list_devices` | Enumerates Apple-Family-7+ Metal devices. Returns device count or `-ENOSYS` when built without Metal. |

Typical call sequence:

```text
vmaf_init()
vmaf_metal_state_init(&state, cfg)     ← new
vmaf_metal_import_state(vmaf, state)   ← new; hands state to ctx
loop:
  vmaf_metal_picture_import(state, iosurface, plane, w, h, bpc, is_ref, index)
  vmaf_metal_wait_compute(state)
  vmaf_metal_read_imported_pictures(vmaf, index)
vmaf_score_pooled(vmaf, ...)
vmaf_close(vmaf)                       ← retry on nonzero; only 0 invalidates
vmaf_metal_state_free(&state)         ← caller frees only after exact 0
```

### IOSurface zero-copy import (ADR-0423)

For FFmpeg/VideoToolbox callers that hold `CVPixelBufferRef`-backed frames, the
fork ships a zero-copy IOSurface import path. The caller pulls the `IOSurface`
via `CVPixelBufferGetIOSurface` and hands it to libvmaf; the Metal feature
kernels read the frame without a host round-trip.

| Symbol | Description |
| --- | --- |
| `VmafMetalExternalHandles` | Struct carrying an `id<MTLDevice>` and `id<MTLCommandQueue>` as `uintptr_t` to keep the header Metal-framework-free. |
| `vmaf_metal_state_init_external` | Allocates a `VmafMetalState` that adopts caller-supplied Metal handles instead of creating its own device/queue. Required when the IOSurface source and libvmaf compute must share the same `MTLDevice`. |
| `vmaf_metal_picture_import` | Imports a single plane of an `IOSurfaceRef` (as `uintptr_t`) into the libvmaf Metal pipeline. The caller retains ownership; libvmaf locks the surface read-only and copies the plane into a shared-storage `VmafPicture`. |
| `vmaf_metal_wait_compute` | Blocks until all Metal compute work on `state` has finished. Currently a synchronous no-op (v1 import path is a host-side memcpy); future async paths replace this with an `MTLSharedEvent` drain. |
| `vmaf_metal_read_imported_pictures` | Triggers a libvmaf score read for the imported ref+dis IOSurfaces at `index`. |

Metal currently targets Apple Silicon (Apple-Family-7, M1 and later) and ships
runtime dispatch for 8 feature kernels. VIF, ADM, CIEDE, CAMBI, SSIMULACRA2,
MS-SSIM, PSNR-HVS, and motion3 are tracked as follow-up kernels. Intel Macs are
unsupported unless a future ADR adds a dedicated runtime contract for them.

## Related

- [index.md](index.md) — core API (everything on this page sits on top of it)
- [dnn.md](dnn.md) — tiny-AI session API (separate from classic GPU dispatch)
- [../usage/cli.md#backend-selection](../usage/cli.md#backend-selection) —
  `--no_cuda` / `--no_sycl` / `--sycl_device` flags
- [../backends/cuda/overview.md](../backends/cuda/overview.md),
  [../backends/sycl/overview.md](../backends/sycl/overview.md), and
  [../backends/hip/overview.md](../backends/hip/overview.md) —
  user-facing backend pages
- [../usage/bench.md](../usage/bench.md) — `vmaf_bench`, which consumes
  these APIs to produce the perf + validation tables
- [ADR-0016](../adr/0016-sycl-to-master-merge-conflict-policy.md),
  [ADR-0022](../adr/0022-inference-runtime-onnx.md),
  [ADR-0027](../adr/0027-non-conservative-image-pins.md) —
  governing decisions

## Licensing of the GPU headers (ADR-1250)

`libvmaf_sycl.h`, `libvmaf_cuda.h`, `libvmaf_hip.h` and `libvmaf_metal.h` are
<!-- REUSE-IgnoreStart -->
fork-authored and carry `SPDX-License-Identifier: EUPL-1.2`. Linking against
<!-- REUSE-IgnoreEnd -->
them is use of the library, not modification: the reciprocity applies when you
redistribute a **modified** libvmaf. See
[ADR-1250](../adr/1250-eupl-fork-relicense.md) and the
[README](../../README.md#upstream-and-license).
