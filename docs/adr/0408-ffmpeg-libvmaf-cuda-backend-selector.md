# ADR-0408: FFmpeg libvmaf filter — CUDA backend selector

- **Status**: Accepted
- **Date**: 2026-05-09
- **Deciders**: lusoris
- **Tags**: ffmpeg-patches, cuda, integration

## Context

FFmpeg's stock `libvmaf` filter consumes software AVFrames and runs the
fork's CPU feature kernels. The fork has had a CUDA backend in libvmaf
since the upstream Netflix import; it is reachable from the libvmaf CLI
via `--cuda` and from the dedicated `libvmaf_cuda` filter (CUDA hwaccel
frames in). What it has *not* been reachable from is the regular
`libvmaf` filter on software input — even though the SYCL and Vulkan
backends shipped per-context selectors for exactly that case in patches
[0003](../../ffmpeg-patches/0003-libvmaf-wire-sycl-backend-selector.patch)
and [0004](../../ffmpeg-patches/0004-libvmaf-wire-vulkan-backend-selector.patch).
A user with a CUDA-only build of libvmaf, software-decoded input, and no
desire to wire up the CUDA hwaccel path on FFmpeg's side has no way to
ask the libvmaf filter for CUDA acceleration.

The configure surface mirrors the asymmetry: `--enable-libvmaf-sycl` and
`--enable-libvmaf-vulkan` exist as user-facing flags (added by the same
0003/0004 patches), but `--enable-libvmaf-cuda` does not — `libvmaf_cuda`
is auto-detected at configure time alongside the dedicated
`libvmaf_cuda` filter and is not in `EXTERNAL_LIBRARY_LIST`.

## Decision

We will add a new patch `0010-libvmaf-wire-cuda-backend-selector.patch`
to the `ffmpeg-patches/` series that:

1. Exposes `--enable-libvmaf-cuda` as an `EXTERNAL_LIBRARY_LIST` entry,
   matching `--enable-libvmaf-sycl` / `--enable-libvmaf-vulkan`.
2. Adds a `cuda` boolean AVOption (default `0`) on the `libvmaf` filter.
   When `cuda=1` the filter inits a `VmafCudaState` against the CUDA
   primary context on the default device (selected by
   `CUDA_VISIBLE_DEVICES` at process scope, matching the libvmaf CLI
   `--cuda` flag — the upstream C-API's `VmafCudaConfiguration` has no
   device-index field, unlike SYCL/Vulkan), imports the state into the
   `VmafContext`, preallocates a `HOST_PINNED` `VmafPicture` pool on
   first frame, and dispenses pictures from the pool so the existing
   copy loop fills pinned-host memory.
3. Coexists with the upstream dedicated `libvmaf_cuda` filter via the
   `CONFIG_LIBVMAF_CUDA && !CONFIG_LIBVMAF_CUDA_FILTER` guard. When
   both are configured the libvmaf-filter selector logs an error and
   refuses init rather than fighting the dedicated filter's `cu_state`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Re-use the dedicated `libvmaf_cuda` filter only | Zero new surface in FFmpeg | Forces users onto FFmpeg's CUDA hwaccel decode path; misses software-input → CUDA-feature use case that SYCL/Vulkan already handle on the libvmaf filter | Asymmetric with SYCL/Vulkan; user request explicitly cites closing this gap |
| Add an `int cuda_device` integer instead of a boolean | Mirrors the SYCL/Vulkan `_device` shape | `VmafCudaConfiguration` has no `device_index` field; the value would be ignored or require a fork-only API extension | Boolean honestly represents the upstream C-API; `CUDA_VISIBLE_DEVICES` is the documented selector at process scope |
| Use `DEVICE` preallocation instead of `HOST_PINNED` | Avoids any host-side staging | The existing copy loop fills `dst->data[i]` with `memcpy` — `DEVICE` buffers would force a staging round-trip before kernel launch, defeating the purpose | `HOST_PINNED` matches Vulkan's `HOST` and lets the copy loop fill pinned memory directly; downstream CUDA kernels DMA without an extra hop |
| Keep `libvmaf_cuda` as auto-detected (no `EXTERNAL_LIBRARY_LIST` entry) | Smaller configure diff | Asymmetric with SYCL/Vulkan; user explicitly asked for `--enable-libvmaf-cuda` | The user-facing flag is the deliverable |

## Consequences

- **Positive**:
  - Closes the FFmpeg integration gap; CUDA is now a peer of SYCL and
    Vulkan on the regular `libvmaf` filter.
  - `--enable-libvmaf-cuda` exists alongside its siblings; configure
    `--help` advertises all three symmetrically.
  - Fail-soft: when libvmaf was built without CUDA the configure probe
    silently disables `CONFIG_LIBVMAF_CUDA`; the filter still builds,
    `cuda=1` then errors at filter-init time with `AVERROR(ENOSYS)`.
- **Negative**:
  - Promoting `libvmaf_cuda` from blanket-autodetect to
    `EXTERNAL_LIBRARY_LIST` means users who previously got the in-filter
    CUDA selector "for free" with `--enable-libvmaf` now need
    `--enable-libvmaf-cuda` explicitly. Acceptable for symmetry; the
    dedicated `libvmaf_cuda` filter is unaffected (its
    `_filter_deps="libvmaf libvmaf_cuda ffnvcodec"` chain still
    auto-resolves when all three are present).
  - The dedicated-filter coexistence guard (`error: cuda=1 set on the
    libvmaf filter, but FFmpeg was built with the dedicated
    libvmaf_cuda filter`) reads as a footgun until the user discovers
    they should pick one or the other.
- **Neutral / follow-ups**:
  - When `VmafCudaConfiguration` ever grows a `device_index` field
    upstream, swap the boolean for an integer `cuda_device` mirroring
    SYCL/Vulkan; track this as a follow-up rebase note.
  - Consider hoisting the picture-pool wiring into a shared helper
    once a third backend (HIP, Metal) lands a similar selector — at
    that point the `if (s->vulkan_state) … else if (s->cu_state) …`
    chain in `copy_picture_data` becomes worth refactoring.

## References

- Patch series: `ffmpeg-patches/series.txt` and
  [`ffmpeg-patches/README.md`](../../ffmpeg-patches/README.md).
- Sibling backends:
  [ADR-0118](0118-ffmpeg-patch-series-application.md) (series replay
  gate), [ADR-0186](0186-vulkan-image-import-impl.md) (Vulkan import
  with per-frame pool),
  [ADR-0238](0238-vulkan-picture-preallocation.md)
  (lazy pool init pattern this ADR copies).
- libvmaf CUDA C-API:
  [`core/include/libvmaf/libvmaf_cuda.h`](../../core/include/libvmaf/libvmaf_cuda.h),
  ownership contract documented in
  [`core/src/cuda/AGENTS.md`](../../core/src/cuda/AGENTS.md).
- Source: `req` (user task — close the FFmpeg integration gap so
  `--enable-libvmaf-cuda` is exposed alongside the existing
  `--enable-libvmaf-sycl` / `--enable-libvmaf-vulkan`).
