# VMAFx device frames and fences

A device frame is a picture held in memory that a compute device reads directly,
such as decoded video in GPU memory, capture card buffers, or host memory mapped
for compute ([ADR-1829](../../adr/1829-rc4-zero-copy-import.md)). The VMAFx API
imports these buffers without copying them through the host, ordering reads and
reuses with explicit fences ([ADR-1852](../../adr/1852-vmafx-api-redesign.md)
section 2.7, [ADR-1929](../../adr/1929-vmafx-device-frames-fences.md)).

Declarations are in `vmafx/frame.h` and `vmafx/device.h`. The generated reference
pages list every enumerator and struct field ([frame.md](frame.md),
[device.md](device.md)).

## When to import instead of copy

When an upstream producer (a hardware video decoder, a capture card, a rendering
pipeline, or an upstream GPU filter) already holds frames in memory that a device
can read, creating a host frame with `vmafx_frame_create_host()` and copying
pixels into it wastes CPU cycles and PCIe bandwidth.

`vmafx_frame_import()` binds the producer's memory directly:

- **Zero copy by default**: Planar formats matching the device's alignment
  requirements are bound in place without allocation or data movement
  ([ADR-1929](../../adr/1929-vmafx-device-frames-fences.md)).
- **Device-side planarisation**: Semi-planar formats (NV12, P010, P016), packed
  layouts (AYUV, UYVY422, V210), and MSB-aligned layouts are converted on the
  device with GPU kernels (de-interleaving, bit-shifting, and gather masks)
  rather than bouncing through host memory
  ([ADR-2133](../../adr/2133-vmafx-import-422-444-formats.md),
  [ADR-2145](../../adr/2145-vmafx-input-format-table.md)).
- **Host memory without copy**: On the CPU device, `VMAFX_MEMORY_HOST` binds the
  caller's allocation directly (`core/test/test_vmafx_import_api.c`).
- **Never a silent host copy**: If a device cannot read an imported frame
  directly or planarise it on the device, the import is refused by name. The
  library never falls back to downloading device frames to the host
  ([ADR-1929](../../adr/1929-vmafx-device-frames-fences.md)).

## Complete host import example

The following example opens a CPU device, imports a planar 8-bit YUV420P host
frame with an explicit acquire fence, retrieves the release fence, releases the
frame reference, waits for the release fence, and cleans up all objects.

<!-- compile -->
```c
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <vmafx/vmafx.h>

int main(void)
{
    const uint32_t width = 64;
    const uint32_t height = 48;
    const size_t y_size = (size_t)width * (size_t)height;
    const size_t uv_size = y_size / 4u;
    const size_t total_bytes = y_size + 2u * uv_size;

    uint8_t *pixels = (uint8_t *)calloc(total_bytes, sizeof(uint8_t));
    if (!pixels) {
        return EXIT_FAILURE;
    }

    VmafxDevice *device = NULL;
    VmafxError *error = NULL;
    VmafxStatus status = vmafx_device_create(NULL, &device, &error);
    if (status != VMAFX_OK) {
        vmafx_error_free(error);
        free(pixels);
        return EXIT_FAILURE;
    }

    VmafxFence acquire_fence = VMAFX_FENCE_INIT;
    status = vmafx_fence_create(device, VMAFX_FENCE_HOST, &acquire_fence, &error);
    if (status != VMAFX_OK) {
        vmafx_error_free(error);
        vmafx_device_unref(device);
        free(pixels);
        return EXIT_FAILURE;
    }

    /* Producer writes pixel data and signals acquire fence. */
    status = vmafx_fence_signal(&acquire_fence, &error);
    if (status != VMAFX_OK) {
        vmafx_error_free(error);
        VmafxError *dest_err = NULL;
        if (vmafx_fence_destroy(&acquire_fence, &dest_err) != VMAFX_OK) {
            vmafx_error_free(dest_err);
        }
        vmafx_device_unref(device);
        free(pixels);
        return EXIT_FAILURE;
    }

    VmafxFrameImport desc = VMAFX_FRAME_IMPORT_INIT;
    desc.memory = VMAFX_MEMORY_HOST;
    desc.pix_fmt = VMAFX_PIXEL_FORMAT_YUV420P;
    desc.bpc = 8;
    desc.w = width;
    desc.h = height;
    desc.n_planes = 3;

    desc.plane[0].handle = (uintptr_t)pixels;
    desc.plane[0].pitch = width;
    desc.plane[0].offset = 0;
    desc.plane[0].size = y_size;

    desc.plane[1].handle = (uintptr_t)(pixels + y_size);
    desc.plane[1].pitch = width / 2u;
    desc.plane[1].offset = 0;
    desc.plane[1].size = uv_size;

    desc.plane[2].handle = (uintptr_t)(pixels + y_size + uv_size);
    desc.plane[2].pitch = width / 2u;
    desc.plane[2].offset = 0;
    desc.plane[2].size = uv_size;

    desc.acquire = acquire_fence;
    desc.flags = 0; /* 0 = zero copy only */

    VmafxFrame *frame = NULL;
    status = vmafx_frame_import(device, &desc, &frame, &error);

    /* Acquire fence is borrowed for import; caller destroys its handle. */
    VmafxError *acq_err = NULL;
    if (vmafx_fence_destroy(&acquire_fence, &acq_err) != VMAFX_OK) {
        vmafx_error_free(acq_err);
    }

    if (status != VMAFX_OK) {
        vmafx_error_free(error);
        vmafx_device_unref(device);
        free(pixels);
        return EXIT_FAILURE;
    }

    VmafxFence release_fence = VMAFX_FENCE_INIT;
    status = vmafx_frame_release_fence(frame, VMAFX_FENCE_HOST, &release_fence, &error);
    if (status != VMAFX_OK) {
        vmafx_error_free(error);
        vmafx_frame_unref(frame);
        vmafx_device_unref(device);
        free(pixels);
        return EXIT_FAILURE;
    }

    /* Submitting to contexts consumes frame references. When all contexts
     * finish reading, dropping references releases the frame. */
    vmafx_frame_unref(frame);

    /* Producer waits on release fence before reusing or freeing memory. */
    status = vmafx_fence_wait(&release_fence, UINT64_MAX, &error);
    if (status != VMAFX_OK) {
        vmafx_error_free(error);
        VmafxError *rel_err = NULL;
        if (vmafx_fence_destroy(&release_fence, &rel_err) != VMAFX_OK) {
            vmafx_error_free(rel_err);
        }
        vmafx_device_unref(device);
        free(pixels);
        return EXIT_FAILURE;
    }

    status = vmafx_fence_destroy(&release_fence, &error);
    if (status != VMAFX_OK) {
        vmafx_error_free(error);
        vmafx_device_unref(device);
        free(pixels);
        return EXIT_FAILURE;
    }

    vmafx_device_unref(device);
    free(pixels);
    return EXIT_SUCCESS;
}
```

## The import descriptor

An import is configured with `VmafxFrameImport` and `VmafxImportPlane`
([frame.md](frame.md)). Always initialise the struct with
`VMAFX_FRAME_IMPORT_INIT`, which sets `struct_size` and zeros every field,
including nested fences:

```c
VmafxFrameImport desc = VMAFX_FRAME_IMPORT_INIT;
```

### Memory kinds

`desc.memory` specifies what each plane handle refers to (`VmafxMemoryKind`):

| Kind | Handle / descriptor | Where valid |
| --- | --- | --- |
| `VMAFX_MEMORY_HOST` | Virtual address of the plane | CPU |
| `VMAFX_MEMORY_DEVICE_POINTER` | CUDA/HIP device pointer, SYCL USM pointer | CUDA, HIP, SYCL |
| `VMAFX_MEMORY_DEVICE_ARRAY` | CUDA or HIP array handle | CUDA, HIP |
| `VMAFX_MEMORY_DMABUF` | Linux dma-buf file descriptor (`plane.fd`) | SYCL, HIP |
| `VMAFX_MEMORY_METAL_SURFACE` | `IOSurfaceRef` object handle | Declared; refused today (see [Metal](#metal)) |
| `VMAFX_MEMORY_METAL_TEXTURE` | `id<MTLTexture>` object handle | Declared; refused today (see [Metal](#metal)) |
| `VMAFX_MEMORY_WIN32_SHARED` | Shared NT texture handle | Refused by SYCL until a Windows device runs its tests ([ADR-2091](../../adr/2091-vmafx-sycl-device-frames.md)) |
| `VMAFX_MEMORY_GL_TEXTURE` | OpenGL 2D texture name (`GL_TEXTURE_2D`) | CUDA, HIP, SYCL |
| `VMAFX_MEMORY_VULKAN` | Exported memory fd or NT handle | CUDA, HIP, SYCL |

`VMAFX_MEMORY_NONE` (0) is refused with `VMAFX_E_INVALID`.

### Geometry and planes

- `desc.pix_fmt`: The layout of the producer's planes (`VmafxPixelFormat`).
- `desc.bpc`: Bit depth per sample (8 to 16, as format permits).
- `desc.w`, `desc.h`: Frame luma width and height in pixels.
- `desc.n_planes`: Number of planes in `desc.plane` (1 for mono and packed
  formats, 2 for semi-planar formats, 3 for planar formats).
- `desc.plane[i]`: Embedded `VmafxImportPlane` describing plane `i`:
  - `handle`: Memory pointer, device address, array, surface, or texture.
  - `fd`: File descriptor for dma-buf or Vulkan memory (duplicated by the
    library; the caller's descriptor remains open and caller-owned).
  - `plane_index`: Plane index within a multi-plane surface or container.
  - `offset`: Byte offset from allocation start to the plane's first sample.
  - `pitch`: Byte stride between consecutive rows.
  - `modifier`: DRM format modifier (0 = linear layout).
  - `size`: Total allocation size in bytes (required for dma-buf and Vulkan).

### Flags: zero copy only

`desc.flags` controls copy behaviour:

- `0` (the default from `VMAFX_FRAME_IMPORT_INIT`): Strict zero copy. Any layout
  that the device cannot bind directly without copying is refused
  ([ADR-1929](../../adr/1929-vmafx-device-frames-fences.md) item 5).
- `VMAFX_IMPORT_ALLOW_COPY`: Permits a device-to-device copy when a device
  cannot bind the layout directly (e.g. planar frames in CUDA arrays). Even with
  this flag, the library never copies device memory through the host.

### RGB layouts require explicit colour metadata

For `VMAFX_PIXEL_FORMAT_RGB`, `RGBA`, and `BGRA`, colour properties cannot be
guessed. `desc.rgb_matrix`, `desc.rgb_range`, `desc.rgb_transfer`, and
`desc.rgb_out_range` must be explicitly specified
([ADR-2146](../../adr/2146-vmafx-rgb-input-explicit-matrix.md)). Passing
`VMAFX_COLOR_MATRIX_UNKNOWN` (0) or an unsupported matrix (such as constant
luminance or ICtCp) fails with `VMAFX_E_INVALID` or `VMAFX_E_NOTSUP` naming the
unspecified field.

## Fence ownership and synchronisation

Synchronisation between the caller and the engine is managed through `VmafxFence`
structures initialised with `VMAFX_FENCE_INIT` ([frame.md](frame.md)).

### Acquire fence

`desc.acquire` orders work before the engine reads the frame:

- The producer signals the acquire fence when its write commands complete.
- `vmafx_frame_import()` borrows the acquire fence. For GPU backends, it
  registers a device-side wait on the library stream or queue.
- Because the fence is borrowed, the caller may destroy its own fence handle
  immediately after `vmafx_frame_import()` returns
  (`core/test/test_vmafx_import_api.c`).
- `VMAFX_FENCE_NONE` indicates that planes are already written and ready.
- `desc.acquire_more[2]` accommodates multi-plane timeline semaphores (e.g.
  Vulkan imports with an independent timeline per plane,
  [ADR-2152](../../adr/2152-vmafx-vulkan-frame-import.md)).

### Release fence

`vmafx_frame_release_fence(frame, kind, &out, &error)` returns a fence signalled
once the library has finished reading the frame:

- Call while holding a reference to the frame, before submitting it.
- Each call returns a newly allocated fence object owned by the caller.
- Signalled only when the frame's last reference is dropped across all contexts
  it was submitted to ([ADR-1906](../../adr/1906-vmafx-core-api-semantics.md)
  item 4, [ADR-1929](../../adr/1929-vmafx-device-frames-fences.md)).
- The caller destroys every returned fence with `vmafx_fence_destroy()`.

### Release callback

`desc.release` (`VmafxFrameReleaseCallback`) and `desc.user`:

- Called once on whichever thread drops the frame's last reference, after all
  release fences are enqueued or recorded.
- Allows GPU producers to schedule a stream wait on a `CUDA_EVENT`, `HIP_EVENT`,
  or `SYCL_EVENT` release fence directly on the GPU without blocking the host
  ([ADR-2023](../../adr/2023-vmafx-cuda-device-frames.md) item 4).

### Host fences

Host fences are reference-counted synchronization flags managed by the library
([ADR-1929](../../adr/1929-vmafx-device-frames-fences.md) item 2):

- **Creation**: `vmafx_fence_create(device, VMAFX_FENCE_HOST, &out, &error)`.
  Pass `NULL` for the CPU device.
- **Signalling**: `vmafx_fence_signal(&fence, &error)`.
- **Waiting and Polling**: `vmafx_fence_wait(&fence, timeout_ns, &error)`:
  - `timeout_ns = 0` polls without blocking: returns `VMAFX_OK` if signalled,
    or `VMAFX_PENDING` without generating an error or logging a failure.
  - `timeout_ns > 0` waits up to the deadline: returns `VMAFX_E_TIMEOUT`
    naming the fence if time expires before signalling.
  - `UINT64_MAX` waits indefinitely until signalled.
- **Destruction**: `vmafx_fence_destroy(&fence, &error)` drops the caller's
  reference and releases underlying resources.

### Producer signal on release

For external sync primitives such as Vulkan timeline semaphores,
`vmafx_frame_signal_on_release(frame, &signal_fence, &error)` registers a fence
that the device will signal on the device timeline behind the last reader
([ADR-2152](../../adr/2152-vmafx-vulkan-frame-import.md)).

## Retention depth and pool sizing

Contexts retain reference frames across multiple submissions to compute temporal
metrics such as motion ([ADR-1478](../../adr/1478-motion-five-frame-window-port.md)).

### Retention depth

`vmafx_context_frame_retention(context)` queries how many earlier reference
frames the context keeps after `vmafx_submit()` returns ([context.md](context.md)):

- `1`: Standard temporal extractors keeping frame $n-1$.
- `2`: Features reading frame $n-2$ (e.g. 5-frame motion window).
- `0`: When `context` is `NULL` or holds no temporal extractors.

### Frames in flight and pool sizing

`vmafx_context_max_in_flight(context)` returns the upper bound on the number of
frames of each input held by the context at any moment
([ADR-2074](../../adr/2074-vmafx-window-scores.md)):

$$\text{max\_in\_flight} = R + 2 \times T \times (R + 1) + (1 \text{ on GPU backends})$$

where $R$ is `vmafx_context_frame_retention(context)` and $T$ is the number of
worker threads (`n_threads`).

- **Without worker threads** ($T = 0$): A submit finishes scoring before
  returning, holding only $R$ retained reference frames.
- **With worker threads** ($T > 0$): Submissions apply backpressure when $T$
  frames await worker processing, keeping at most $2 \times T \times (R + 1)$
  frames in flight.
- **Device backends**: Device feature extractors double-buffer execution,
  holding 1 additional frame until the next submit collects results.

A producer that recycles buffers through a fixed ring or pool must size the
pool to at least:

$$\text{pool\_size} \ge \text{vmafx\_context\_max\_in\_flight}(context) + 1$$

The additional frame provides buffer space for the producer while all in-flight
slots are occupied. Preallocate reusable frames using `vmafx_frame_pool_create()`
([frame.md](frame.md)). Pool exhaustion returns `VMAFX_E_BUSY` until in-flight
frames are released.

## The named-refusal rule

The core invariant of the import system is: **imported frames never take a host
copy** ([docs/development/rebase-sensitive-invariants.md](../../development/rebase-sensitive-invariants.md),
[ADR-1929](../../adr/1929-vmafx-device-frames-fences.md)).

If an import cannot be fulfilled cleanly as requested, it fails with an explicit
error code naming the problematic subject in `vmafx_error_subject()`:

- Unsupported memory kind or fence kind: `VMAFX_E_NOTSUP` naming the kind.
- Unsupported pixel format or bit depth: `VMAFX_E_INVALID` naming `desc.pix_fmt`
  or `desc.bpc`.
- A pitch too small for the plane: `VMAFX_E_INVALID` naming
  `desc.plane[i].pitch`; a plane that does not fit its allocation:
  `VMAFX_E_RANGE`; a nonzero modifier on linear memory: `VMAFX_E_NOTSUP`.
- Unspecified RGB color parameters: `VMAFX_E_INVALID` naming the field
  ([ADR-2146](../../adr/2146-vmafx-rgb-input-explicit-matrix.md)).
- Vulkan memory from another GPU: `VMAFX_E_NOTSUP` naming `desc.vulkan_pci`.
- Incompatible feature extractors: `vmafx_context_admit()` returns
  `VMAFX_E_NOTSUP` naming each registered extractor that cannot read the frame
  natively without host copies.

### The D8 helper

`vmafx_context_import_frame()` implements the complete admission and import rule
([ADR-1852](../../adr/1852-vmafx-api-redesign.md) decision D8,
[ADR-1929](../../adr/1929-vmafx-device-frames-fences.md) item 8):

1. Executes `vmafx_frame_import()` and `vmafx_context_admit()`.
2. If an acquire fence is not yet ready (`VMAFX_E_BUSY` or `VMAFX_E_TIMEOUT`),
   it performs a single host wait on `desc.acquire` for up to
   `import_retry_wait_ns` (configured in `VmafxContextConfig`, default 10 s)
   and retries once.
3. If the retry fails or any refusal occurs, it reports a failure naming the
   input label (`main` or `reference`), backend, device, memory kind, pixel
   format, modifiers, and the refusing extractors.
4. It never clears the zero-copy requirement or falls back to a host copy.

## Backend-specific rules

Each compute backend binds a specific subset of memory and fence kinds, enforcing
strict device stream and queue ordering.

### CUDA

- **Memory kinds**: `VMAFX_MEMORY_DEVICE_POINTER`, `VMAFX_MEMORY_DEVICE_ARRAY`,
  `VMAFX_MEMORY_GL_TEXTURE`, `VMAFX_MEMORY_VULKAN` (opaque fd).
- **Fence kinds**: `VMAFX_FENCE_CUDA_EVENT`, `VMAFX_FENCE_HOST`,
  `VMAFX_FENCE_GL_SYNC`, `VMAFX_FENCE_VULKAN_SEMAPHORE`.
- **Stream rule**: The device owns one non-blocking library stream. All kernel
  launches, conversions, and release events are enqueued on this stream.
  `CUDA_EVENT` acquire fences trigger `cuStreamWaitEvent()` on the stream.
  Release events are recorded on the library stream when the frame's last
  reference drops, and the release callback executes immediately after
  recording ([ADR-2023](../../adr/2023-vmafx-cuda-device-frames.md)).

### HIP

- **Memory kinds**: `VMAFX_MEMORY_DEVICE_POINTER`, `VMAFX_MEMORY_DMABUF`,
  `VMAFX_MEMORY_DEVICE_ARRAY`, `VMAFX_MEMORY_GL_TEXTURE` (via EGL dma-buf
  export, [ADR-2132](../../adr/2132-hip-gl-textures-through-egl-dmabuf.md)),
  `VMAFX_MEMORY_VULKAN` (dma-buf route, linear).
- **Fence kinds**: `VMAFX_FENCE_HIP_EVENT`, `VMAFX_FENCE_HOST`,
  `VMAFX_FENCE_SYNC_FILE`, `VMAFX_FENCE_GL_SYNC`.
- **Stream rule**: The device creates one non-blocking library stream. The
  feature twins copy device picture memory on this stream, and extractor
  streams synchronize with it. `HIP_EVENT` acquire fences wait on the library
  stream; `SYNC_FILE` and `GL_SYNC` fences are polled on the host before
  submission ([ADR-2092](../../adr/2092-vmafx-hip-device-frames.md)).

### SYCL

- **Memory kinds**: `VMAFX_MEMORY_DEVICE_POINTER` (device, shared, or host USM
  allocated in the device context), `VMAFX_MEMORY_DMABUF` (Intel Tile4,
  Y-tiled, linear), `VMAFX_MEMORY_GL_TEXTURE` (EGL dma-buf),
  `VMAFX_MEMORY_VULKAN` (dma-buf route, linear or Tile4).
- **Fence kinds**: `VMAFX_FENCE_SYCL_EVENT`, `VMAFX_FENCE_HOST`,
  `VMAFX_FENCE_SYNC_FILE`, `VMAFX_FENCE_GL_SYNC`,
  `VMAFX_FENCE_VULKAN_SEMAPHORE`.
- **Queue rule**: The device owns one in-order library queue with immediate
  command lists. Synchronisation uses kernel joins (`depends_on()`), never
  `ext_oneapi_submit_barrier()`. Reader kernels copy planes on their own queues
  waiting on the frame's ready event. The release fence is a join over all
  reader events ([ADR-2091](../../adr/2091-vmafx-sycl-device-frames.md)).

### Metal

- **Memory kinds**: `VMAFX_MEMORY_METAL_SURFACE` (`IOSurfaceRef`) and
  `VMAFX_MEMORY_METAL_TEXTURE` (`id<MTLTexture>`) are declared in
  `vmafx/frame.h`, but `vmafx_frame_import()` has no Metal lane yet: it
  imports only on the CPU and on the CUDA, SYCL and HIP devices of the build,
  and refuses every other device with `VMAFX_E_NOTSUP`.
- **Today's path**: IOSurface frames are imported through the deprecated
  libvmaf call `vmaf_metal_picture_import()` (`libvmaf_metal.h`), which plans
  bi-planar NV12 and P010 surfaces itself: chroma de-interleaved, P010 samples
  shifted down by 6
  ([ADR-1679](../../adr/1679-metal-iosurface-biplanar-import.md)).

### Vulkan (import-only)

- **Memory kinds**: `VMAFX_MEMORY_VULKAN`.
- **Fence kinds**: `VMAFX_FENCE_VULKAN_SEMAPHORE`.
- **Routing rule**: VMAFx contains no Vulkan compute engine and links no Vulkan
  loader. Vulkan frames exported by external producers are imported into CUDA,
  SYCL, or HIP compute devices ([ADR-2152](../../adr/2152-vmafx-vulkan-frame-import.md)).
  CUDA imports opaque-fd memory and waits on timeline semaphores on its stream.
  SYCL and HIP import linear or DRM-modifier memory via the dma-buf route and
  synchronize via `sync_file` or host fences. Imports across different GPUs
  (`desc.vulkan_pci`), multi-plane single-image allocations, and OPTIMAL
  tiling on SYCL/HIP are refused by name
  ([ADR-3353](../../adr/3353-hip-keeps-refusing-vulkan-optimal.md)).
