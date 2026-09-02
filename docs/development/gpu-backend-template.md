# GPU Backend Public-API Template

When adding a new GPU backend (Metal, DirectML, OpenCL, future
ROCm-replacement, …), follow the shape the existing four backends
(`libvmaf_cuda.h`, `libvmaf_sycl.h`, `libvmaf_vulkan.h`, `libvmaf_hip.h`)
already share. This doc is the recipe — there is **no codegen**;
the dedup-via-pattern lives here.

> **Why no codegen?** A 2026-05-02 audit measured the four headers
> at ~20 of ~200 lines truly shared (state lifecycle). The rest is
> backend-specific feature surface (CUDA: preallocation; SYCL: DMABuf
> / VA-surface / D3D11; Vulkan: VkImage zero-copy import + ring depth;
> HIP: scaffold only). Codegenning 10 % of each file would add a
> build-system Python dependency for too little return. ADR-0239
> chose pattern-doc over codegen.

## Shared lifecycle (every GPU backend ships these)

```c
typedef struct Vmaf<Backend>State Vmaf<Backend>State;

typedef struct Vmaf<Backend>Configuration {
    int device_index;            /* -1 = first compatible device */
    /* Backend-specific config fields here. Keep additions additive
     * (zero-initialised structs must compile + run correctly). */
} Vmaf<Backend>Configuration;

/**
 * Allocate a Vmaf<Backend>State. Picks the device by index; -1 selects
 * the first device that exposes the required compute/queue capability.
 *
 * @return 0 on success, -ENOSYS when built without <Backend> support,
 *         -ENODEV when no compatible device is found, -EINVAL on bad
 *         arguments.
 */
int vmaf_<backend>_state_init(Vmaf<Backend>State **out, Vmaf<Backend>Configuration cfg);

/**
 * Hand the state to a VmafContext. The context borrows the state pointer
 * for its lifetime; the caller still owns the state and must free it
 * with vmaf_<backend>_state_free() after vmaf_close().
 */
int vmaf_<backend>_import_state(VmafContext *ctx, Vmaf<Backend>State *state);

/**
 * Release a state previously allocated via vmaf_<backend>_state_init.
 * Safe to pass NULL or a state that was never imported.
 *
 * Two valid signatures exist across the existing four backends:
 *
 *     void vmaf_<backend>_state_free(Vmaf<Backend>State **state);   // Vulkan, HIP
 *     int  vmaf_<backend>_state_free(Vmaf<Backend>State *state);    // CUDA
 *
 * Pick the void/double-pointer form for new backends — it lets the
 * function NULL the caller's pointer (CUDA's int-return form is a
 * historical quirk inherited from upstream Netflix and exists in
 * fewer call sites; new backends should not replicate it).
 */
void vmaf_<backend>_state_free(Vmaf<Backend>State **state);
```

## Optional: device enumeration

Backends whose device set is dynamic / runtime-detected (Vulkan, HIP,
future Metal) ship a `_list_devices` helper that prints one line per
device with ordinal + name + capability. Backends with fixed device
selection (CUDA via index) skip this.

```c
/**
 * Enumerate compute-capable devices visible to the runtime. Prints
 * one line per device with its ordinal, name, and capability.
 * @return device count, or -ENOSYS when built without <Backend> support.
 */
int vmaf_<backend>_list_devices(void);
```

## Optional: build-time availability probe

Backends conditionally compiled via a meson option ship an
`_available()` query so callers can branch on backend presence
without linking against the symbol table directly. Currently
shipped by Vulkan and HIP.

```c
/**
 * Returns 1 if libvmaf was built with <Backend> support, 0 otherwise.
 * Cheap to call; no <Backend> runtime is touched until
 * vmaf_<backend>_state_init().
 */
int vmaf_<backend>_available(void);
```

## Optional: picture preallocation surface (CUDA / SYCL / Vulkan)

When the backend wants callers to write directly into the buffers the
kernel will read (avoiding a host → device staging copy), expose the
preallocation pool. The shape mirrors the SYCL surface (the cleanest
of the three; CUDA's `HOST_PINNED` is CUDA-allocator-specific and has
no analogue elsewhere — don't replicate it):

```c
enum Vmaf<Backend>PicturePreallocationMethod {
    VMAF_<BACKEND>_PICTURE_PREALLOCATION_METHOD_NONE = 0,
    VMAF_<BACKEND>_PICTURE_PREALLOCATION_METHOD_HOST,
    VMAF_<BACKEND>_PICTURE_PREALLOCATION_METHOD_DEVICE,
};

typedef struct Vmaf<Backend>PictureConfiguration {
    struct {
        unsigned w, h;
        unsigned bpc;
        enum VmafPixelFormat pix_fmt;
    } pic_params;
    enum Vmaf<Backend>PicturePreallocationMethod pic_prealloc_method;
} Vmaf<Backend>PictureConfiguration;

int vmaf_<backend>_preallocate_pictures(VmafContext *vmaf,
                                         Vmaf<Backend>PictureConfiguration cfg);
int vmaf_<backend>_picture_fetch(VmafContext *vmaf, VmafPicture *pic);
```

The implementation MUST delegate to the backend-agnostic
`VmafGpuPicturePool` (`core/src/gpu_picture_pool.{h,c}`) per
ADR-0239 — do not reimplement the round-robin / mutex / unwind
shape. Each backend supplies the alloc / free / synchronize callbacks

- a per-pool cookie carrying its state pointer.

## Optional: zero-copy hwaccel import paths

Backends with a path for *adopting* externally-decoded GPU memory
(SYCL DMABuf / VA-surface / D3D11; Vulkan VkImage) ship a separate
import surface that varies enough per backend that no pattern is
forced — design it backend-natively. Document the lifetime model
(who owns the source handle, who owns the imported state, when it's
safe to free the source).

## Doxygen + ABI stability conventions

- Every public function carries a Doxygen block with `@return` listing
  every error code path, including `-ENOSYS` for the
  built-without-backend case.
- Configuration structs grow **additive only**. Zero-initialised
  structs from older callers must continue to compile + run with
  default behaviour. This is enforced project-wide; new fields go at
  the end of the struct, never in the middle.
- Opaque state types (`Vmaf<Backend>State`) are forward-declared in
  the public header; their layout lives in
  `core/src/<backend>/<backend>_internal.h` (or equivalent
  per-backend internal header) so kernel TUs can read the device /
  queue / allocator handles without crossing the public surface.
- Public ABI changes (renames, removed entry points,
  signature shape changes) are forbidden without an ADR + a
  matching `ffmpeg-patches/` update per CLAUDE.md §12 r14.

## Internal-side companion files (NOT in this header)

The corresponding backend-internal files follow their own pattern:

```text
core/src/<backend>/
  common.{c,h}              # state init / device enumeration / queue setup
  picture_<backend>.{c,h}   # buffer / picture allocation
  dispatch_strategy.{c,h}   # per-feature dispatch helpers
  <backend>_internal.h      # opaque struct layouts for kernel TUs
```

The `gpu_picture_pool.{c,h}` round-robin is **shared** — every backend
that wants a preallocation pool delegates to it (ADR-0239).

The feature kernel host glue — what every `<feature>_<backend>.c`
under `core/src/feature/<backend>/` ships — has its own boilerplate
extraction in flight as PR4 of the GPU dedup sequence (T-GPU-DEDUP-3,
~250/500 LOC shared per file × 10+ files × 3 backends).

## See also

- [ADR-0239](../adr/0239-gpu-picture-pool-dedup.md) — backend-agnostic
  GPU picture pool (PR2 of the dedup sequence).
- [ADR-0238](../adr/0238-vulkan-picture-preallocation.md) — Vulkan
  picture preallocation (the most recent shape adopter).
- [ADR-0250](../adr/0250-tiny-ai-extractor-template.md) — tiny-AI
  extractor template (the model for "pattern-doc + shared helpers
  rather than codegen").
- [`core/include/libvmaf/AGENTS.md`](../../core/include/libvmaf/AGENTS.md)
  — the public-headers-tree invariant note that points back here.
