# ADR-0238: Vulkan VmafPicture preallocation surface (API parity with CUDA / SYCL)

- **Status**: Accepted
- **Date**: 2026-05-02
- **Deciders**: Lusoris
- **Tags**: vulkan, api, preallocation, fork-local, parity

## Context

The CUDA backend ships `vmaf_cuda_preallocate_pictures` /
`vmaf_cuda_fetch_preallocated_picture` (with HOST / HOST_PINNED /
DEVICE methods); the SYCL backend ships
`vmaf_sycl_preallocate_pictures` / `vmaf_sycl_picture_fetch` (HOST /
DEVICE). Vulkan ships nothing — `docs/api/gpu.md` flagged this as a
known limitation:

> Per-feature picture preallocation API is **not** yet exposed —
> Vulkan kernels allocate device-side VkBuffers internally per
> feature. The CUDA/SYCL-style `VmafVulkanPicturePreallocationMethod`
> surface is a follow-up.

The gap blocks two real use cases:

1. **Driver-style integration**: a caller that owns its decode loop
   wants to write source frames directly into the buffers libvmaf's
   compute kernels will read, removing the host → device staging
   copy. Without preallocation, every frame round-trips through the
   per-extractor internal buffers.
2. **Memory budgeting**: in a long-running pipeline (FFmpeg per-shot
   tooling, encoder-side ROI scoring), pinning the picture pool depth
   up front lets operators reason about the resident set instead of
   tracking per-feature internal allocators.

Closing the parity gap is purely additive — the existing import-image
zero-copy path (ADR-0186 / ADR-0251) remains the right answer for
external-VkImage callers; preallocation serves the host-driven path.

## Decision

We will ship `VmafVulkanPicturePreallocationMethod` (NONE / HOST /
DEVICE) and the matching `vmaf_vulkan_preallocate_pictures` +
`vmaf_vulkan_picture_fetch` entry points, modelled on the SYCL surface
(the SYCL pool is the simpler reference; CUDA's HOST_PINNED option
maps to a CUDA-only allocator and has no Vulkan analogue).

`HOST` allocates pictures via the regular `vmaf_picture_alloc` —
useful when the score consumer is CPU-side. `DEVICE` backs each
picture's luma plane with a host-visible Vulkan buffer (VMA
`AUTO_PREFER_HOST`); the persistent mapped pointer is exposed as
`pic->data[0]`, so the caller writes once and the kernel descriptor
sets read the same memory. A new
`VMAF_PICTURE_BUFFER_TYPE_VULKAN_DEVICE` tags these pictures so
cross-backend extractors can refuse mixed backings.

Pool depth is fixed at the canonical `frames-in-flight = 2` (matches
SYCL's compile-time constant); the depth is not part of the public
surface in this PR. ADR-0251's `max_outstanding_frames` knob is
unrelated — that controls the import-image fence ring, not the
preallocation pool. A separate follow-up can grow the configuration
struct if a real workload needs depth > 2.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Mirror SYCL surface (chosen)** | Shortest review path; SYCL pool is already battle-tested in the FFmpeg integration; same enum names + same pool-depth contract | Inherits SYCL's "luma-only" pool (chroma allocation deferred) | Picked: API parity is the goal; the chroma deferral is consistent across SYCL + Vulkan and tracked separately |
| Mirror CUDA surface (HOST / HOST_PINNED / DEVICE) | Three methods feels more complete; CUDA users see familiar names | HOST_PINNED has no Vulkan analogue (VMA `AUTO_PREFER_HOST` is the closest, but it's not "pinned" in the CUDA sense) | Rejected: would ship a method we'd then have to caveat as "treated like HOST" |
| Skip the public surface; expose a fork-internal helper | Smaller blast radius; less ABI commitment | Doesn't close the parity gap noted in `docs/api/gpu.md`; FFmpeg integrators still bounce off the missing surface | Rejected: the gap is explicit and visible |
| Bake preallocation into `vmaf_vulkan_state_init` (auto-pool-on-import) | One fewer call site for callers | Forces every Vulkan caller to pay the pool cost; breaks the import-image zero-copy path which deliberately doesn't pre-allocate | Rejected: opt-in is the right ergonomics |
| Add a `pic_cnt` parameter to the configuration struct in this PR | Configurable from day one | Premature; SYCL's hard-coded 2 has shipped without complaint; growing the struct later is additive | Rejected: ship the minimal surface, grow it if a workload demands depth > 2 |

## Consequences

- **Positive**:
  - Closes the CUDA / SYCL / Vulkan API parity gap for picture
    preallocation, removing the explicit "follow-up" caveat from
    `docs/api/gpu.md`.
  - Lets FFmpeg-side host-driven integrations (the non-zero-copy
    path) write source frames directly into the buffers libvmaf
    compute kernels read, removing one host → device staging copy.
  - The new `VMAF_PICTURE_BUFFER_TYPE_VULKAN_DEVICE` tag enables
    cross-backend extractors to refuse mixed-backing inputs in
    future work.
  - Pool tear-down lives in a single hook in `vmaf_close()`,
    matching the SYCL pattern; no per-extractor cleanup needed.
- **Negative**:
  - One additional translation unit
    (`core/src/vulkan/picture_vulkan_pool.c`, ~180 LOC).
  - Public ABI grows two entry points + one enum + one struct.
    Backwards compatibility is preserved (additive only).
  - The pool currently allocates only the Y plane — same as SYCL.
    Chroma-aware extractors that want pre-allocated U/V planes
    (none of the current Vulkan kernels do) need a follow-up.
- **Neutral / follow-ups**:
  - Pool depth tunable. The current 2 matches SYCL's compile-time
    constant; if a workload needs depth > 2, grow the
    `VmafVulkanPictureConfiguration` struct additively (mirroring
    ADR-0251 follow-up #3's pattern).
  - Chroma plane preallocation. Out of scope here; tracked as a
    separate item once a Vulkan kernel actually consumes
    pre-allocated chroma.
  - External-handles parity. The pool currently uses the imported
    state's VkInstance / VkDevice via the fork-internal
    `vmaf_vulkan_state_context()` accessor. External callers
    (`vmaf_vulkan_state_init_external`) work transparently; no
    additional plumbing required.
  - Cross-backend parity gate stays unchanged at `places=4` —
    preallocation only changes where the buffer lives, not which
    bytes the kernel reads.

## FFmpeg-side adoption (deferred, by design)

CLAUDE.md §12 r14 requires `ffmpeg-patches/` updates when libvmaf
adds an entry point **that the patches consume**. None of the in-tree
patches (`0002` cuda / `0003` sycl / `0004` vulkan-selector / `0006`
libvmaf_vulkan) call any `*_preallocate_pictures` /
`*_picture_fetch` entry point today — neither for CUDA nor for SYCL.
Those filters go through the regular per-frame `vmaf_picture_alloc`
host path. Switching them to consume the preallocation pool is a
separate optimization with two distinct angles:

- **`vf_libvmaf_vulkan` (zero-copy import path, patch `0006`)**: this
  filter receives external VkImages via `vmaf_vulkan_import_image`
  and never allocates VmafPictures itself. Preallocation is
  *orthogonal* — no change needed, ever.
- **`vf_libvmaf` host path with `vulkan_device=N`**: currently
  allocates host-backed pictures per frame. Switching to
  `vmaf_vulkan_preallocate_pictures(DEVICE)` would let FFmpeg's
  frame data land directly in the kernel-read buffer. Real perf win,
  but it's a separate PR with its own benchmark gate (mirrors the
  pattern of ADR-0251's measurement-gate flip). For symmetry with
  SYCL — which also ships preallocation but whose FFmpeg filter
  doesn't consume it yet — we deliberately keep this PR's scope at
  "API parity surface" rather than "FFmpeg integration
  optimization". When the FFmpeg adoption PR lands, it's
  additionally gated on demonstrating no host → device staging
  copy in `perf record` traces against an unchanged Netflix
  golden score.

`ffmpeg-patches/series.txt` is unchanged by this PR; the
`apply --check` gate against the pinned `n8.1` baseline passes
unmodified.

## References

- Source: `req` 2026-05-02 — popup answer "do 1 first then 4"
  (test_speed fix, then per-feature picture preallocation for Vulkan).
- [`docs/api/gpu.md`](../api/gpu.md) — flagged the gap.
- [`core/include/libvmaf/libvmaf_cuda.h`](../../core/include/libvmaf/libvmaf_cuda.h)
  — CUDA reference surface (`VmafCudaPicturePreallocationMethod`).
- [`core/include/libvmaf/libvmaf_sycl.h`](../../core/include/libvmaf/libvmaf_sycl.h)
  — SYCL reference surface (chosen mirror).
- [`core/src/sycl/picture_sycl.cpp`](../../core/src/sycl/picture_sycl.cpp)
  — SYCL pool implementation (the model for `picture_vulkan_pool.c`).
- [ADR-0186](0186-vulkan-image-import-impl.md) — the existing
  zero-copy import path; preallocation is a parallel surface, not a
  replacement.
- [ADR-0251](0251-vulkan-async-pending-fence.md) — `max_outstanding_frames`
  knob. Orthogonal to this ADR (controls the fence ring, not the
  picture pool).
- Companion research digest: [`docs/research/0045-vulkan-picture-preallocation.md`](../research/0045-vulkan-picture-preallocation.md).

### Status update 2026-05-08: Accepted

Audited as part of the 2026-05-08 ADR `Proposed` sweep
([Research-0086](../research/0086-adr-proposed-status-sweep-2026-05-08.md)).

Acceptance criteria verified in tree at HEAD `0a8b539e`:

- `core/include/libvmaf/libvmaf_vulkan.h` declares
  `VmafVulkanPicturePreallocationMethod` (line 153),
  `VmafVulkanPictureConfiguration::pic_prealloc_method` (line 165),
  `vmaf_vulkan_preallocate_pictures` (line 180),
  `vmaf_vulkan_picture_fetch` (line 191).
- Verification command:
  `grep -nE "VmafVulkanPicturePreallocationMethod|vmaf_vulkan_preallocate_pictures|vmaf_vulkan_picture_fetch"
  core/include/libvmaf/libvmaf_vulkan.h`.
