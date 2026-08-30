<!-- markdownlint-disable MD060 -->
# ADR-0380: FFmpeg libvmaf filter — HIP backend selector patch (0011)

- **Status**: Accepted
- **Date**: 2026-05-10
- **Deciders**: lusoris
- **Tags**: ffmpeg-patches, hip, integration

## Context

The fork ships FFmpeg integration patches for every GPU backend on the
regular `libvmaf` filter:

| Backend | Selector patch | Dedicated-filter patch |
|---------|---------------|------------------------|
| SYCL    | 0003 (`sycl_device` option) | 0005 (`libvmaf_sycl`) |
| Vulkan  | 0004 (`vulkan_device` option) | 0006 (`libvmaf_vulkan`) |
| CUDA    | 0010 (`cuda` boolean) | — (upstream `libvmaf_cuda` filter) |
| **HIP** | **missing** | **missing** |

The HIP backend is real: `core/include/libvmaf/libvmaf_hip.h` is a
public C-API header with `vmaf_hip_state_init` / `vmaf_hip_import_state`
/ `vmaf_hip_state_free`. PRs #695 / #696 / #710 landed the runtime; 6 of
11 HIP kernels are real post-PR-#710 / ADR-0375. CLAUDE.md §12 r14
(the ffmpeg-patches surface gate) requires the patch stack to track every
libvmaf C-API surface.

A dedicated `libvmaf_hip` filter (analogous to `libvmaf_vulkan` or
`libvmaf_sycl`) is the natural second patch, but FFmpeg has no
ROCm/HIP hwdec path that produces `AVFrame`s with HIP-native device
pointers. There is no `ffhipcodec` equivalent of CUDA's `ffnvcodec`,
and no VA-API / DMA-BUF → HIP zero-copy path exposed through FFmpeg's
hwcontext layer at this time. Shipping a dedicated filter now would be
an empty shell with no hardware-frame input to consume.

## Decision

We will add **one patch** to the `ffmpeg-patches/` series:

**`0011-libvmaf-wire-hip-backend-selector.patch`** — mirrors the shape
of patch 0003 (SYCL) and patch 0004 (Vulkan):

1. Adds `--enable-libvmaf-hip` to `configure`'s external-library help
   text and to `EXTERNAL_LIBRARY_LIST`, making it a peer of
   `--enable-libvmaf-sycl`, `--enable-libvmaf-vulkan`, and
   `--enable-libvmaf-cuda`.
2. Adds a `hip_device` integer AVOption (default `-1` = disabled) on the
   `libvmaf` filter. When `hip_device >= 0` the filter calls
   `vmaf_hip_state_init()` with the user-supplied device index, then
   `vmaf_hip_import_state()`, then frees via `vmaf_hip_state_free()` on
   cleanup (double-pointer free, per the `libvmaf_hip.h` contract).
3. Degrades gracefully: when `hip_device >= 0` but libvmaf was built
   without HIP (`CONFIG_LIBVMAF_HIP` absent from the configure probe),
   the filter logs a clear error and returns `AVERROR(ENOSYS)`.

The picture path uses `vmaf_picture_alloc()` (standard host allocation)
rather than a pinned-memory pool because the HIP public API does not yet
expose `vmaf_hip_preallocate_pictures` / `vmaf_hip_fetch_preallocated_picture`
equivalents of the CUDA pair. When that surface lands, patch 0011 can be
extended — or a new patch 0013 can add the pool path — without breaking
the existing `hip_device` option wire.

A dedicated `libvmaf_hip` filter patch (0012) is **deferred** until
FFmpeg exposes a ROCm/HIP hardware-frame context that can supply
device-side frame pointers to `vmaf_hip_import_state`. The deferral is
documented in `docs/state.md` and `docs/rebase-notes.md`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|--------|------|------|----------------|
| Bundle HIP into an updated `libvmaf_cuda` filter rather than a separate `hip_device` option | Fewer patches, single filter | Conflicts with the SYCL/Vulkan precedent where each backend has its own selector; also the dedicated `libvmaf_cuda` filter already owns the `cuda` boolean and splitting the two options across filters confuses users | Each backend gets its own named option, matching the established pattern |
| Use a `hip` boolean (like the `cuda` boolean in 0010) instead of a `hip_device` integer | Closer analogy to 0010 | The HIP C-API `VmafHipConfiguration` already has a `device_index` field (-1 = first device); exposing it as an integer is more useful and matches SYCL/Vulkan | Integer option chosen to expose `device_index` directly |
| Ship both 0011 (selector) and 0012 (filter) now | Complete HIP surface | The dedicated filter requires an FFmpeg hwdec path that does not yet exist; shipping an empty shell would be misleading | Deferred to avoid premature scaffolding without a real consumer |
| Do nothing — wait for FFmpeg upstream to add HIP support | No patch maintenance burden | CLAUDE.md §12 r14 requires tracking libvmaf C-API surfaces; the HIP API is real and the gap is a rule violation | Not an option; the rule is clear |

## Consequences

- **Positive**: `--enable-libvmaf-hip` now exists alongside its three
  siblings; `./configure --help` advertises all four symmetrically.
  Users with an AMD GPU and a HIP-enabled libvmaf can pass
  `libvmaf=hip_device=0` to the `libvmaf` filter to route computation
  through the HIP backend.
- **Positive**: Closes the CLAUDE.md §12 r14 gap; the FFmpeg patch
  stack tracks the public `libvmaf_hip.h` C-API surface.
- **Positive**: The `hip_device` integer option exposes `device_index`
  natively, allowing multi-GPU AMD systems to target a specific ordinal
  without setting environment variables.
- **Negative**: No picture-pool path yet — the SYCL/Vulkan pool's
  zero-staging-copy benefit is unavailable for HIP until
  `vmaf_hip_preallocate_pictures` ships. Software frames go through the
  standard `vmaf_picture_alloc()` host allocation before the HIP backend
  processes them.
- **Neutral / follow-ups**:
  - When `vmaf_hip_preallocate_pictures` / `vmaf_hip_fetch_preallocated_picture`
    land in `libvmaf_hip.h`, extend patch 0011 (or add a thin patch 0013)
    to wire the HOST_PINNED preallocation pool, matching the CUDA selector in
    patch 0010.
  - When FFmpeg grows a ROCm/HIP hwdec path, ship patch 0012
    (`libvmaf_hip` dedicated filter for zero-copy device-frame import).
  - The ADR-0350 note about refactoring `copy_picture_data`'s backend
    chain when a third backend lands a selector now applies: consider
    extracting a shared `ensure_picture_pool()` helper in a future PR.

## References

- Patch series: `ffmpeg-patches/series.txt` and
  [`ffmpeg-patches/README.md`](../../ffmpeg-patches/README.md).
- Sibling backends:
  [ADR-0118](0118-ffmpeg-patch-series-application.md) (series replay gate),
  [ADR-0350](0350-ffmpeg-libvmaf-cuda-backend-selector.md) (CUDA selector),
  [ADR-0186](0186-vulkan-image-import-impl.md) (Vulkan dedicated filter),
  [ADR-0238](0238-vulkan-picture-preallocation.md) (lazy pool init pattern).
- HIP C-API: [`core/include/libvmaf/libvmaf_hip.h`](../../core/include/libvmaf/libvmaf_hip.h).
- HIP runtime: ADR-0212 (scaffold), ADR-0372 / 0373 / 0375 (real kernels).
- CLAUDE.md §12 r14 (ffmpeg-patches surface gate — triggering rule).
- req: user directive to add missing HIP FFmpeg integration patches for
  `libvmaf_hip.h` C-API surfaces per CLAUDE.md §12 r14.
