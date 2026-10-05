<!-- markdownlint-disable MD013 -->
# GPU backends C API

Use this page to add a GPU backend to a libvmaf context from C. Each
backend (CUDA, SYCL, HIP, Metal) adds a small header on top of
[`libvmaf.h`](index.md): a state object, device selection, and
backend-specific picture handling. The Vulkan backend was removed
([ADR-0726](../adr/0726-drop-vulkan-backend.md)); no Vulkan entry point
exists.

Related pages: [core API overview](index.md),
[CLI backend selection](../usage/cli.md#backend-selection),
[backend dispatch rules](../backends/index.md).

## When these headers apply

| Backend | Header | Build options | Without the option |
| --- | --- | --- | --- |
| CUDA | `libvmaf_cuda.h` | `-Denable_cuda=true` | Header not installed; the symbols do not exist. |
| SYCL | `libvmaf_sycl.h` | `-Denable_sycl=true` | Header not installed; the symbols do not exist. |
| HIP | `libvmaf_hip.h` | `-Denable_hip=true` for the API; `-Denable_hipcc=true` for device kernels | Header not installed; the library still exports stubs that return `-ENOSYS`. |
| Metal | `libvmaf_metal.h` | `-Denable_metal=auto` (default) or `enabled`, on macOS | Header installed when Metal is `enabled` or `auto`; entry points return `-ENOSYS` when the build has no Metal, `-ENODEV` on an unsupported device. |

Registered feature extractors per backend, from the extractor registry:

| Backend | Extractors | Where the list is |
| --- | --- | --- |
| HIP | 19 | [HIP overview](../backends/hip/overview.md) |
| Metal | 17 (every extractor except SpEED) | [Metal backend](../backends/metal/index.md) |

!!! note
    libvmaf does not export `HAVE_CUDA`, `HAVE_HIP` and similar macros
    through `pkg-config`. To write code that builds against any libvmaf,
    probe at build time in your own build system, or call
    `vmaf_hip_available()` / `vmaf_metal_available()` at run time.

!!! note "Toolchain minimums"
    HIP needs ROCm 7.0 or newer (`core/meson_options.txt`); the shipped
    toolchain is ROCm 10.1.0 (`build-config.env`).

## Ownership at a glance

All four backends keep the caller responsible for the state allocation.
They differ in what `import_state` copies.

| Backend | `import_state` | Who frees the state | When | Free call |
| --- | --- | --- | --- | --- |
| CUDA | Copies `VmafCudaState` by value into the context | Caller frees the original allocation | After `vmaf_close()` returned 0 | `vmaf_cuda_state_free(state)` (plain pointer, not cleared) |
| SYCL | Borrows the pointer | Caller | After `vmaf_close()` returned 0 | `vmaf_sycl_state_free(&state)` |
| HIP | Borrows the pointer | Caller | After `vmaf_close()` returned 0 | `vmaf_hip_state_free(&state)` |
| Metal | Borrows the pointer | Caller | After `vmaf_close()` returned 0 | `vmaf_metal_state_free(&state)` |

Common rules:

- Call `vmaf_close()` first. Only an exact `0` tears down what the context
  holds; on a nonzero result retry it and keep every state and model alive
  ([close and retry](lifecycle.md#close-and-retry)).
- A successful CUDA close destroys the embedded copy (stream and, if
  libvmaf created it, the context). The original allocation still needs
  `vmaf_cuda_state_free()` afterwards or it leaks.
- Import the state before `vmaf_use_features_from_model()` and before the
  first `vmaf_read_pictures()`.
- A state belongs to one context. CUDA returns `-EBUSY` for a second import
  of the same state or an import into a context that already has CUDA state.
- Every function in these headers is not thread-safe; use one state and one
  context per driver thread.
- ABI: fork-added, stable. The SYCL zero-copy imports are experimental.

## CUDA

Header:
[`libvmaf_cuda.h`](../../core/include/libvmaf/libvmaf_cuda.h).

### Call order

```text
vmaf_init()
vmaf_cuda_state_init()
vmaf_cuda_import_state()               hands a by-value copy to the context
vmaf_use_features_from_model()
vmaf_cuda_preallocate_pictures()
loop:
  vmaf_cuda_fetch_preallocated_picture() x2
  fill .data[i]
  vmaf_read_pictures()
vmaf_read_pictures(NULL, NULL, 0)
vmaf_score_pooled()
vmaf_close()                           retry on nonzero
vmaf_cuda_state_free()                 only after close returned 0
```

### State and configuration

```c
typedef struct VmafCudaConfiguration {
    void *cu_ctx;   /* CUcontext; NULL: libvmaf creates one */
} VmafCudaConfiguration;

int vmaf_cuda_state_init(VmafCudaState **cu_state, VmafCudaConfiguration cfg);
int vmaf_cuda_import_state(VmafContext *vmaf, VmafCudaState *cu_state);
int vmaf_cuda_state_free(VmafCudaState *cu_state);
```

| Field value | Effect |
| --- | --- |
| `cu_ctx = NULL` | libvmaf creates a fresh context on the current CUDA device (device 0 by default). Typical for standalone tools. |
| `cu_ctx != NULL` | A driver-API `CUcontext` the application owns, for example one shared with NVENC or NVDEC. It must outlive the state and every importing context until `vmaf_close()` returns 0. |

Errors: `vmaf_cuda_import_state()` returns `-EBUSY` as described above, or
another negative errno. `vmaf_cuda_state_free()` accepts `NULL`.

### Ownership and free

`vmaf_cuda_state_free()`
([ADR-0157](../adr/0157-cuda-preallocation-leak-netflix-1300.md))
frees the original allocation. When the state was never imported it first
tears down the stream and context, which is retry-safe: a nonzero result
retains the state and its live handles for another call.

```c
VmafCudaState *cuda = NULL;
int err = vmaf_cuda_state_init(&cuda, (VmafCudaConfiguration){ .cu_ctx = NULL });
if (err)
    return err;

err = vmaf_cuda_import_state(ctx, cuda);   /* ctx holds a by-value copy */
if (err) {
    vmaf_cuda_state_free(cuda);            /* never imported: tears down too */
    return err;
}

/* ... score ... */

int close_err = vmaf_close(ctx);
if (close_err != 0)
    close_err = vmaf_close(ctx);           /* retained teardown-only context */
if (close_err == 0)
    vmaf_cuda_state_free(cuda);            /* original allocation outlives close */
```

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

int vmaf_cuda_preallocate_pictures(VmafContext *vmaf, VmafCudaPictureConfiguration cfg);
int vmaf_cuda_fetch_preallocated_picture(VmafContext *vmaf, VmafPicture *pic);
```

| Method | `.data[i]` memory | Behaviour of `vmaf_cuda_fetch_preallocated_picture` | Use when |
| --- | --- | --- | --- |
| `NONE` | none | Returns `-EINVAL`: no pool exists. Allocate pictures yourself. | Diagnostics only. |
| `DEVICE` | `cudaMalloc` | Takes a picture from a device pool. | Source data already on the GPU (decoder output); no host-to-device copy in `vmaf_read_pictures`. |
| `HOST` | `malloc` | Allocates a fresh pageable host picture each call. | Host source; libvmaf copies through a bounce buffer. |
| `HOST_PINNED` | `cudaMallocHost` | Allocates a fresh pinned host picture each call. | Host source with asynchronous copy overlap; the usual choice for CPU-decoded feeds. |

Preallocating twice on one context returns `-EBUSY`. Pictures from a
fetch follow the normal [ownership rule](pictures.md#the-rule-in-one-paragraph):
after `vmaf_read_pictures()` the context owns them. The
[CUDA backend page](../backends/cuda/overview.md) has measured
throughput.

### Example

```c
#include <libvmaf/libvmaf.h>
#include <libvmaf/libvmaf_cuda.h>
#include <libvmaf/model.h>

static int run(VmafContext *vmaf, VmafModel *model, unsigned nframes, double *score)
{
    VmafCudaPictureConfiguration pcfg = {
        .pic_params = { .w = 1920, .h = 1080, .bpc = 8, .pix_fmt = VMAF_PIX_FMT_YUV420P },
        .pic_prealloc_method = VMAF_CUDA_PICTURE_PREALLOCATION_METHOD_HOST_PINNED,
    };
    int err = vmaf_use_features_from_model(vmaf, model);
    if (err) return err;
    err = vmaf_cuda_preallocate_pictures(vmaf, pcfg);
    if (err) return err;

    for (unsigned i = 0; i < nframes; i++) {
        VmafPicture ref, dist;
        err = vmaf_cuda_fetch_preallocated_picture(vmaf, &ref);
        if (err) return err;
        err = vmaf_cuda_fetch_preallocated_picture(vmaf, &dist);
        if (err) { vmaf_picture_unref(&ref); return err; }
        /* fill ref.data[p] / dist.data[p] row by row with stride[p] */
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        if (err) return err;               /* the context owns both pictures */
    }
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    if (err) return err;
    return vmaf_score_pooled(vmaf, model, VMAF_POOL_METHOD_MEAN, score, 0, nframes - 1);
}
```

Surround `run()` with `vmaf_init()`, `vmaf_cuda_state_init()`,
`vmaf_cuda_import_state()` and the close sequence above.

### Limitations

- One device per state. `VmafCudaConfiguration` has no device index: select
  device N by making its context current (`cuCtxSetCurrent` or
  `cudaSetDevice`) before `vmaf_cuda_state_init()`, or pass its `CUcontext`.
- No stream parameter. libvmaf runs its own streams; an external stream is
  not exposed.

## SYCL

Header:
[`libvmaf_sycl.h`](../../core/include/libvmaf/libvmaf_sycl.h).

### State and configuration

```c
typedef struct VmafSyclConfiguration {
    int device_index;        /* -1: SYCL default device; 0+: Level Zero GPU ordinal */
    int enable_profiling;    /* non-zero: create the queue with profiling */
} VmafSyclConfiguration;

int  vmaf_sycl_state_init(VmafSyclState **out, VmafSyclConfiguration cfg);
int  vmaf_sycl_import_state(VmafContext *ctx, VmafSyclState *state);
void vmaf_sycl_state_free(VmafSyclState **state);
int  vmaf_sycl_list_devices(void);
```

- The state owns the `sycl::queue`, device allocations, shared frame buffers
  and profiling data. SYCL USM allocations are queue-scoped and the queue
  outlives one scoring session, which is why the caller frees the state
  after `vmaf_close()` returns 0.
- `vmaf_sycl_list_devices()` enumerates `device_type::gpu` devices only and
  prints one line each: ordinal, platform, vendor, driver version, fp64
  support. It returns the count, or `-EIO` on a SYCL exception.
  `vmaf_bench --list-devices` uses it ([bench usage](../usage/bench.md)).

### Picture preallocation

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

The enumerator values are stable and append-only; serialised configuration
and FFI bindings may rely on them. `DEVICE` and `HOST` create a 2-deep pool.

| Method | Backing | Use case |
| --- | --- | --- |
| `NONE` | No pool; `vmaf_sycl_picture_fetch` falls back to `vmaf_picture_alloc()` | CPU-fed callers and test harnesses |
| `DEVICE` | `sycl::malloc_device` USM | A decoder or uploader writes directly into GPU-resident planes |
| `HOST` | `sycl::malloc_host` USM | CPU-visible pooled planes |

The caller owns each picture reference returned by
`vmaf_sycl_picture_fetch()`. Submit it through `vmaf_read_pictures()`, then
release the caller's reference with `vmaf_picture_unref()`. The pool keeps
its own references until `vmaf_close()` returns 0; after a nonzero close the
context and its pool stay retained for retry.

### Zero-copy frame-buffer path

For a GPU-resident decode pipeline (Intel VPL, VA-API, D3D11) the
frame-buffer API exposes two shared Y-plane buffers (reference and
distorted) and the SYCL backend's double-buffered upload path, instead of
whole `VmafPicture` instances from the pool.

```c
int vmaf_sycl_init_frame_buffers(VmafContext *ctx, unsigned w, unsigned h, unsigned bpc);
int vmaf_sycl_get_frame_buffers (VmafContext *ctx, void **ref, void **dis);
int vmaf_read_pictures_sycl     (VmafContext *ctx, unsigned index);  /* replaces vmaf_read_pictures */
int vmaf_sycl_wait_compute      (VmafContext *ctx);
int vmaf_flush_sycl             (VmafContext *ctx);                  /* replaces the NULL flush */
```

```c
vmaf_sycl_init_frame_buffers(vmaf, W, H, 8);
void *ref_buf = NULL, *dis_buf = NULL;
vmaf_sycl_get_frame_buffers(vmaf, &ref_buf, &dis_buf);

for (unsigned i = 0; i < nframes; i++) {
    /* write Y-plane luma into ref_buf / dis_buf: kernels, SYCL events,
     * dmabuf imports, whatever the decoder exposes */
    vmaf_read_pictures_sycl(vmaf, i);
    /* vmaf_sycl_wait_compute() is needed only to reuse the buffers for a
     * later frame while compute is still in flight */
}
vmaf_flush_sycl(vmaf);
```

The path holds the **luma plane only**, and the extractors get no host picture
([ADR-1688](../adr/1688-sycl-zero-copy-luma-only-admission.md)). Each call to
`vmaf_read_pictures_sycl()` first checks every registered feature extractor.
It returns `-ENOTSUP` before counting the frame, with one libvmaf error line
naming each extractor that cannot run on luma alone:

- a SYCL twin that reads chroma or a host picture. That includes
  `speed_chroma_sycl`, which the default model `vmaf_v1.0.16_3d0h` needs for
  `speed_chroma_uv`; `motion_sycl` with `motion_add_uv=true`; and `psnr_sycl`
  or `psnr_hvs_sycl` with `enable_chroma` (their default);
- a CPU extractor: a feature with no SYCL twin, or a twin that cannot honour
  the model's options.

What runs: `adm_sycl`, `vif_sycl`, `motion_sycl`, `motion_v2_sycl`,
`cambi_sycl`, `float_moment_sycl`, and `psnr_sycl` / `psnr_hvs_sycl` with
`enable_chroma=false`. That covers `vmaf_v0.6.1` (`adm2`, `vif`, `motion2`),
whose scores equal the CPU's on this path. Score anything else from host
frames with `vmaf_read_pictures()`. Before ADR-1688 such a feature failed with
a bare `-22`, crashed the process (`float_psnr_sycl`), added the SAD of chroma
that was never imported (`motion_sycl` with `motion_add_uv`), or was dropped
from the result without an error (a CPU extractor).

### GPU-resident import paths

```c
/* Linux, Level Zero */
int  vmaf_sycl_dmabuf_import(VmafSyclState *state, int fd, size_t size, void **ptr);
void vmaf_sycl_dmabuf_free  (VmafSyclState *state, void *ptr);
int  vmaf_sycl_import_va_surface(VmafSyclState *state, void *va_display,
                                 unsigned int va_surface, int is_ref,
                                 unsigned w, unsigned h, unsigned bpc);
int  vmaf_sycl_upload_plane(VmafSyclState *state, const void *src, unsigned pitch,
                            int is_ref, unsigned w, unsigned h, unsigned bpc);

/* Windows only (_WIN32) */
int  vmaf_sycl_import_d3d11_surface(VmafSyclState *state, void *d3d11_device,
                                    void *d3d11_texture, unsigned subresource,
                                    int is_ref, unsigned w, unsigned h, unsigned bpc);
```

| Function | Role | Notes |
| --- | --- | --- |
| `vmaf_sycl_dmabuf_import` | Primitive: turns a DMA-BUF fd into a SYCL device pointer through Level Zero external memory. | Caller keeps the fd. Free the pointer with `vmaf_sycl_dmabuf_free` (`NULL` is a no-op). |
| `vmaf_sycl_import_va_surface` | Convenience wrapper over dmabuf for a VA-API decode feed. Preferred on Linux. | Imports the luma plane only (see the zero-copy path above for what that can score). Falls back to `vaGetImage` plus a host-to-device copy when the DRM-PRIME export fails (older Mesa, proprietary drivers). `bpc` is 8 or 10. |
| `vmaf_sycl_upload_plane` | Platform-agnostic escape hatch: copies a Y plane from host memory with a row pitch. | Use when nothing better works, or as a benchmark baseline. |
| `vmaf_sycl_import_d3d11_surface` | Windows: copies the decoded texture into a staging texture, maps it and uploads the plane through `vmaf_sycl_upload_plane`. | Implemented in `core/src/sycl/d3d11_import.cpp`. `-EINVAL` for NULL arguments, zero size, `bpc` other than 8 or 10, or a texture smaller than `w` by `h`; `-EIO` when the map fails. |

Every import takes `is_ref` (non-zero: reference buffer, otherwise
distorted) and needs `vmaf_sycl_init_frame_buffers()` first. See
[ADR-0016](../adr/0016-sycl-to-master-merge-conflict-policy.md) for how
these APIs landed and the
[SYCL overview](../backends/sycl/overview.md) for the ingestion-path
decision tree.

### Profiling helpers

```c
int  vmaf_sycl_profiling_enable    (VmafSyclState *state);
void vmaf_sycl_profiling_disable   (VmafSyclState *state);
void vmaf_sycl_profiling_print     (VmafSyclState *state);
int  vmaf_sycl_profiling_get_string(VmafSyclState *state, char **out);
```

Request profiling at init: set `VmafSyclConfiguration.enable_profiling = 1`
(or `VMAF_SYCL_PROFILE=1` in the environment). Then use the enable and
disable pair to choose which frame ranges are timed. The SYCL environment
switches (`VMAF_SYCL_PROFILE`, `VMAF_SYCL_TIMING`, `VMAF_SYCL_IMPORT_DEBUG`,
`VMAF_SYCL_CHECKSUM`) are read once, at first use, like `VMAF_SYCL_DISPATCH`;
set them before the first SYCL state is created.

!!! warning
    `vmaf_sycl_profiling_enable()` does not re-create the queue. It only
    sets a flag on the state (`core/src/sycl/common.cpp`). If the queue was
    not created with profiling, the call succeeds but reading kernel event
    timings later throws a `sycl::exception`.

`vmaf_sycl_profiling_get_string()` returns a caller-owned buffer; release it
with `free()`. It matches `vmaf_bench --gpu-profile`
([bench usage](../usage/bench.md#performance-benchmark-default-mode)).

### Limitations

- `dmabuf_import` and `import_va_surface` need the SYCL queue on the Level
  Zero backend (they call `sycl::get_native<ext_oneapi_level_zero>`). On an
  OpenCL-backend SYCL build they throw `sycl::exception`, which the wrapper
  converts to `-EIO`. The log line is generic
  (`SYCL DMA-BUF import exception: <what()>`), so detect the backend up
  front with `sycl::queue::get_backend()` and fall back to
  `vmaf_sycl_upload_plane` instead of parsing the log.
- `vmaf_sycl_init_frame_buffers()` is single-resolution. Changing `w`, `h` or
  `bpc` mid-stream needs `vmaf_close()` and a new init.

## HIP

Header:
[`libvmaf_hip.h`](../../core/include/libvmaf/libvmaf_hip.h). HIP runtime
types (`hipDevice_t`, `hipStream_t`) cross the ABI as `uintptr_t`, so the
header does not include `<hip/hip_runtime.h>`; cast on your side.

### State and configuration

```c
typedef struct VmafHipConfiguration {
    int device_index; /* -1: first compute-capable HIP device */
    int flags;        /* reserved; pass 0 */
} VmafHipConfiguration;

int  vmaf_hip_available(void);
int  vmaf_hip_state_init(VmafHipState **out, VmafHipConfiguration cfg);
int  vmaf_hip_import_state(VmafContext *ctx, VmafHipState *state);
void vmaf_hip_state_free(VmafHipState **state);
int  vmaf_hip_list_devices(void);
```

A zero-initialised configuration selects device 0.

| Function | Does | Errors |
| --- | --- | --- |
| `vmaf_hip_available` | Returns 1 when libvmaf was built with `-Denable_hip=true`, else 0. Touches no HIP runtime. | none |
| `vmaf_hip_state_init` | Allocates a state pinned to one HIP device and its compute stream. | `-ENODEV` no visible HIP device, `-EINVAL` bad arguments or an ordinal the runtime does not have, another negative errno when the HIP runtime fails, `-ENOSYS` built without HIP |
| `vmaf_hip_import_state` | Hands the state to a context; the context borrows the pointer. | `-EINVAL` NULL `ctx` or `state`, `-ENOSYS` built without HIP |
| `vmaf_hip_state_free` | Releases the state and sets `*state` to `NULL`. Accepts `NULL` or a never-imported state. | none |
| `vmaf_hip_list_devices` | Logs ordinal, name and GFX architecture per device at `VMAF_LOG_LEVEL_INFO`. Returns the count, 0 when the runtime sees no device. | a negative errno when the HIP runtime fails or a device cannot be described, `-ENOSYS` built without HIP |

Every HIP twin of a context runs on the state's device: it creates its
device resources there, and libvmaf makes that device current before each
frame's HIP work and before the flush. The thread that calls
`vmaf_read_pictures()` does not change the device.

### Ownership and call order

After `vmaf_hip_import_state()` the caller still owns the state. Call
`vmaf_hip_state_free(&state)` only after `vmaf_close()` returned 0; a
nonzero close keeps the context and its borrowed state alive for retry. The
state may outlive one scoring session when you run several passes on the
same device.

```text
vmaf_init()
vmaf_hip_state_init(&state, cfg)
vmaf_hip_import_state(vmaf, state)
vmaf_use_features_from_model()
loop: vmaf_read_pictures(vmaf, &ref, &dist, i)
vmaf_read_pictures(NULL, NULL, 0)
vmaf_score_pooled(vmaf, ...)
vmaf_close(vmaf)                     retry on nonzero
vmaf_hip_state_free(&state)          only after close returned 0
```

`libvmaf_hip.h` has no picture-preallocation functions: feed ordinary host
pictures from `vmaf_picture_alloc()` or the [CPU
pool](pictures.md#picture-pools).

### Feature coverage

The registry holds 19 HIP extractors, verified end to end on AMD gfx
hardware: PSNR, float-PSNR, CIEDE, float-moment (which also serves
integer-moment), float-SSIM, integer-SSIM, MS-SSIM, PSNR-HVS, CAMBI,
SSIMULACRA2, integer-motion, integer-motion-v2, float-motion, float-VIF,
integer-VIF, integer-ADM, float-ADM, speed-chroma and speed-temporal. The
older `adm_hip`, `vif_hip` and `motion_hip` stubs no longer exist. The
`float_ansnr` extractor was removed (PR #38) and is registered on no
backend.

The [HIP overview](../backends/hip/overview.md) has the full extractor
table, HSACO fat-binary target selection, build flags, FFmpeg integration
(`hip_device=N`, ADR-0380) and per-kernel notes.

## Metal

Header:
[`libvmaf_metal.h`](../../core/include/libvmaf/libvmaf_metal.h). Metal
types (`id<MTLDevice>`, `id<MTLCommandQueue>`) cross the ABI as
`uintptr_t`. The backend targets Apple Silicon (Apple GPU family 7, M1 and
later); Intel Macs and other hosts return `-ENODEV` instead of falling back
to the CPU ([ADR-0361](../adr/0361-metal-compute-backend.md)). The header is
installed whenever Metal is enabled so FFmpeg's
`--enable-libvmaf-metal` configure probe finds it
([ADR-0437](../adr/0437-metal-public-header-install-and-import-state-declaration.md)).
The registry holds 17
Metal extractors.

### State and configuration

| Function | Does | Errors |
| --- | --- | --- |
| `vmaf_metal_available` | 1 when built with Metal, else 0. | none |
| `vmaf_metal_state_init(&state, cfg)` | Allocates a state for device `device_index` (`-1`: system default; zero-initialised config picks 0). `flags` is reserved. | `-ENODEV` not Apple GPU family 7+, `-ENOSYS` built without Metal, `-EINVAL` |
| `vmaf_metal_state_init_external(&state, handles)` | Adopts caller-supplied `MTLDevice` and `MTLCommandQueue` (`VmafMetalExternalHandles`, `uintptr_t` each; `0` selects the system default device or a new queue). Use when the IOSurface source and libvmaf must share one device. Exclusive with `vmaf_metal_state_init` in one process context. | `-EINVAL`, `-ENODEV`, `-ENOMEM` |
| `vmaf_metal_import_state(ctx, state)` | Hands the state to a context; the context borrows the pointer. | `-EINVAL`, `-ENOSYS` |
| `vmaf_metal_state_free(&state)` | Releases a state from either init function; accepts `NULL`. | none |
| `vmaf_metal_list_devices` | Enumerates Apple GPU family 7+ devices. Returns the count. | `-ENOSYS` built without Metal |

External handles are never owned by libvmaf: keep them alive until
`vmaf_metal_state_free()` returns.

### IOSurface import

For FFmpeg and VideoToolbox callers that hold `CVPixelBufferRef` frames,
take the surface with `CVPixelBufferGetIOSurface` and pass it to libvmaf
([ADR-0423](../adr/0423-metal-iosurface-import-scaffold.md)).

```text
vmaf_init()
vmaf_metal_state_init(&state, cfg)
vmaf_metal_import_state(vmaf, state)
loop:
  vmaf_metal_picture_import(state, iosurface, plane, w, h, bpc, is_ref, index)
  vmaf_metal_wait_compute(state)
  vmaf_metal_read_imported_pictures(vmaf, index)
vmaf_score_pooled(vmaf, ...)
vmaf_close(vmaf)                     retry on nonzero
vmaf_metal_state_free(&state)        only after close returned 0
```

| Function | Does |
| --- | --- |
| `vmaf_metal_picture_import` | Imports one plane (0 = Y, 1 = U, 2 = V) of an `IOSurfaceRef` into a planar 4:2:0 `VmafPicture`. The caller keeps the surface; libvmaf locks it read-only and copies the plane. The surface's pixel format decides the read ([ADR-1679](../adr/1679-metal-iosurface-biplanar-import.md)). NV12 (`420v` / `420f`, `bpc` 8) and P010 (`x420` / `xf20`, `bpc` 10) are what VideoToolbox decodes to: planes 1 and 2 are the Cb and Cr samples of their interleaved second plane, and P010 samples are shifted from the top 10 bits of 16 to the bottom 10. Planar 8-bit 4:2:0 (`y420` / `f420`) is read plane by plane. Errors: `-ENOTSUP` for any other pixel format, `-EINVAL` (including a `bpc` that is not the format's, or a surface plane smaller than the frame), `-EIO` (lock failure), `-ENOMEM`. |
| `vmaf_metal_wait_compute` | Blocks until Metal work on the state finished. Currently a synchronous no-op, because the import is a host-side copy; a later asynchronous path will drain an `MTLSharedEvent`. `-EINVAL` for a NULL state. |
| `vmaf_metal_read_imported_pictures` | Triggers a score read for the imported reference and distorted surfaces at `index`. All three planes (Y, U, V) of both must have been imported at that index. |

!!! note
    The v1 import path copies on the CPU. True zero-copy GPU binding is a
    tracked gap (`GAP-METAL-IOSURFACE-NOT-TRUE-ZERO-COPY`).

## Related

- [Core API overview](index.md), [DNN sessions](dnn.md).
- [CLI backend selection](../usage/cli.md#backend-selection):
  `--no_cuda`, `--no_sycl`, `--sycl_device` and the other flags.
- Backend pages: [CUDA](../backends/cuda/overview.md),
  [SYCL](../backends/sycl/overview.md), [HIP](../backends/hip/overview.md),
  [Metal](../backends/metal/index.md).
- [`vmaf_bench`](../usage/bench.md) consumes these APIs for the performance
  and validation tables.
- Governing decisions:
  [ADR-0016](../adr/0016-sycl-to-master-merge-conflict-policy.md),
  [ADR-0022](../adr/0022-inference-runtime-onnx.md),
  [ADR-0027](../adr/0027-non-conservative-image-pins.md).

## Licensing of the GPU headers (ADR-1250)

<!-- REUSE-IgnoreStart -->
`libvmaf_cuda.h` is Netflix's and carries `SPDX-License-Identifier:
BSD-2-Clause-Patent`. `libvmaf_sycl.h`, `libvmaf_hip.h` and `libvmaf_metal.h`
are fork-authored and carry `SPDX-License-Identifier: EUPL-1.2`.
<!-- REUSE-IgnoreEnd -->
The European Commission reads linking against an EUPL work as creating no
condition on the linking program. Distributing `libvmaf` itself, modified or
not, still obliges you to provide its source or point to a repository that has
it (EUPL-1.2, Article 5). A **modified** libvmaf is distributed under the
EUPL-1.2. See [Embedding VMAFx in another product](../licensing.md#embedding-vmafx-in-another-product),
[ADR-1250](../adr/1250-eupl-fork-relicense.md) and the
[README](../../README.md#upstream-and-license).
