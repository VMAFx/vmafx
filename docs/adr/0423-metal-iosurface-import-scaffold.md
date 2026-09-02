<!-- markdownlint-disable MD060 -->
# ADR-0423: Metal IOSurface zero-copy import (T8-IOS)

- **Status**: Accepted
- **Date**: 2026-05-11
- **Deciders**: kilian, Claude (Anthropic)
- **Tags**: metal, ffmpeg-patches, gpu, t8-ios

## Context

The Metal backend runtime (ADR-0420 / T8-1b) and the first eight
feature kernels (ADR-0421 / T8-1c–j) are live: a caller that hands
libvmaf a software `VmafPicture` can already score it on an Apple
Silicon GPU via the `--backend metal` / `metal_device=` CLI surfaces
(ADR-0422). The remaining gap is the FFmpeg hwdec zero-copy path —
VideoToolbox-decoded `AVFrame`s arrive as
`AV_PIX_FMT_VIDEOTOOLBOX` with `data[3] -> CVPixelBufferRef`, and the
only way to feed them into libvmaf today is the
`hwdownload,format=nv12` round-trip the regular `libvmaf` filter
forces. That defeats the unified-memory zero-copy posture
ADR-0420 paid for on Apple Silicon: every frame round-trips
GPU→host→GPU through `vmaf_picture_alloc` even though the
IOSurface backing the source `CVPixelBufferRef` was already
addressable from any Apple-Family-7 `MTLDevice`.

The Vulkan flavour solved the symmetric problem in
[ADR-0184](0184-vulkan-image-import-scaffold.md) (scaffold) +
[ADR-0186](0186-vulkan-image-import-impl.md) (impl): caller-supplied
`VkImage` handles, an external-init entry point that adopts FFmpeg's
`VkDevice` so the source images are valid on libvmaf's compute device,
plus a dedicated `libvmaf_vulkan` filter
(`ffmpeg-patches/0006-libvmaf-add-libvmaf-vulkan-filter.patch`)
shipping the consumer side. We want the same shape for Metal so the
FFmpeg integration story stays uniform.

Three constraints originally pushed the work toward a two-PR
rollout (scaffold + impl, mirroring ADR-0184 → ADR-0186): API +
patch coupling, obj-c++ review surface, and the missing
`AVMetalDeviceContext` in FFmpeg n8.1.1. The scaffold landed in
this PR's first commit; the impl (originally pencilled in as a
follow-up "T8-IOS-b") was folded into the same PR after review
feedback that an audit-first scaffold with no runtime exercise
on Apple Silicon adds little reviewer value over a single
working-impl PR. The ADR therefore documents the end state
(scaffold + impl in one), keeping the Vulkan two-phase rollout
as the historical reference rather than a hard contract for new
backends.

## Decision

Land the IOSurface import path in a single PR carrying both the
C-API surface and the working implementation:

- **Public API additions** in `core/include/libvmaf/libvmaf_metal.h`:
  `VmafMetalExternalHandles`, `vmaf_metal_state_init_external`,
  `vmaf_metal_picture_import`, `vmaf_metal_wait_compute`,
  `vmaf_metal_read_imported_pictures`. Lifetime + same-device model
  symmetric to the Vulkan import surface (ADR-0184 / ADR-0186).
- **Implementation** in `core/src/metal/picture_import.mm`:
  `IOSurfaceLock(kIOSurfaceLockReadOnly)` + per-row memcpy into a
  shared-storage `VmafPicture` allocated via `vmaf_picture_alloc`.
  Ring depth of 2 slots matches the SYCL preallocation pool and
  the Vulkan v1 default. The texture-direct path
  (`[MTLDevice newTextureWithDescriptor:iosurface:plane:]` /
  `CVMetalTextureCacheCreateTextureFromImage`) remains available
  as a future optimisation if a Metal feature kernel ever needs
  per-sample texture access; v1 routes everything through the
  same VmafPicture host-pointer pipeline the rest of the scoring
  extractors consume, which is the cheapest path on Apple Silicon's
  unified-memory architecture (a Shared-storage MTLBuffer copy and
  a CPU memcpy from a locked IOSurface have the same memory cost).
- **FFmpeg-side filter** in `ffmpeg-patches/0013-libvmaf-add-libvmaf-metal-filter.patch`:
  registers `libvmaf_metal`, accepts `AV_PIX_FMT_VIDEOTOOLBOX`,
  pulls IOSurfaces via `CVPixelBufferGetIOSurface`, routes through
  the new C-API. Passes `handles.device = 0` so libvmaf falls back
  to `MTLCreateSystemDefaultDevice` until upstream FFmpeg ships
  an `AVMetalDeviceContext`; the documented limitation is a
  multi-GPU Mac Pro device-match gap covered by the same-device
  invariant inherited from ADR-0184.
- **Tests** in `core/test/test_metal_smoke.c`: input-validation
  assertions on every new entry point + a
  default-device-success-or-`-ENODEV` skip semantics for
  `vmaf_metal_state_init_external`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Single PR carrying API + filter + runtime + tests | One review, no -ENOSYS dead-code path. | Mixes 3 review domains (C-API, FFmpeg, obj-c++); ADR-0184 precedent shows two-phase reviews catch design issues earlier. | Rejected — Vulkan's two-phase rollout caught the same-device constraint at scaffold time (would have wasted a full impl pass to discover post-hoc). |
| Skip the dedicated filter, extend the regular `libvmaf` filter to consume IOSurfaces when `metal_device >= -1` and input is VideoToolbox | One filter, fewer config knobs. | Couples the metal_device option to a specific hwaccel input format; breaks pixfmt negotiation for the software path (regular `libvmaf` filter expects `AV_PIX_FMT_YUV420P` etc.); diverges from Vulkan / SYCL precedent which uses dedicated filters per hwdec. | Rejected — uniformity with `libvmaf_sycl` / `libvmaf_vulkan` wins. |
| Defer the entire surface until FFmpeg ships `AVMetalDeviceContext` | Avoids the "pick default MTLDevice" hack at `config_props_metal`. | Upstream has no public timeline; we'd block the whole IOSurface story on something we don't control. Single-GPU Apple Silicon Macs (the common case) don't need the device-match guarantee anyway. | Rejected — the default-device pick is a documented limitation with a clean upgrade path. |
| Use `CVMetalTextureCacheCreateTextureFromImage` directly (no IOSurface extraction) | One Apple API instead of two. | The cache requires a `CVPixelBufferRef`; works on macOS but couples libvmaf to CoreVideo. Direct `newTextureWithDescriptor:iosurface:` keeps the C API IOSurface-only and lets non-CoreVideo callers (e.g. ScreenCaptureKit, AVPlayer hooks) import frames too. | Rejected for the public surface; the impl TU may still use the cache internally for biplanar layouts where descriptor chains are cumbersome. |

## Consequences

- **Positive**:
  - Symbol parity with the Vulkan import path: identical entry-point
    shape, lifetime model, and same-device contract.
  - The FFmpeg filter ships now and starts surfacing usage / option
    feedback before the runtime lands. Reviewers can validate the
    user-facing CLI without waiting on obj-c++ review.
  - Lawrence's tap (`lusoris/homebrew-tap`) gets the matching
    `--enable-libvmaf-metal-filter` activation in a separate
    follow-up commit; this scaffold doesn't change the tap.

- **Negative**:
  - `config_props_metal` picks a default MTLDevice until FFmpeg
    exposes `AVMetalDeviceContext`. Multi-GPU Mac Pro hosts may pick
    the wrong device; documented limitation, tracked as a follow-up
    once upstream ships the device-context API.
  - v1 takes a CPU memcpy on every plane (no texture-direct path).
    On Apple Silicon's unified-memory architecture this costs the
    same as a Shared-storage MTLBuffer copy; if a future Metal kernel
    grows a per-sample texture access pattern that benefits from
    the IOSurface backing directly, a follow-up ADR can layer
    `newTextureWithDescriptor:iosurface:plane:` on top without
    changing the public C API.

- **Neutral / follow-ups**:
  - Tap: add `--enable-libvmaf-metal-filter` to `Formula/ffmpeg.rb`
    in `lusoris/homebrew-tap` to activate the filter for Homebrew
    users (separate PR; this one only adds the libvmaf surface +
    FFmpeg patch).
  - GPU-parity gate: no impact — the filter calls the same scoring
    pipeline as the software path, so cross-backend tolerances apply
    unchanged.
  - GPU-parity gate: no impact — the filter calls the same scoring
    pipeline as the software path, so cross-backend tolerances
    apply unchanged once T8-IOS-b lands.

## References

- [ADR-0184](0184-vulkan-image-import-scaffold.md) — Vulkan import
  scaffold precedent (audit-first stubs).
- [ADR-0186](0186-vulkan-image-import-impl.md) — Vulkan import
  implementation precedent (drops the -ENOSYS contract).
- [ADR-0420](0420-metal-backend-runtime-t8-1b.md) — Metal runtime that
  this builds on.
- [ADR-0421](0421-metal-first-kernel-motion-v2.md) — first Metal kernel
  set; defines the `MTLSharedEvent` synchronisation model the import
  path piggybacks on.
- [ADR-0422](0422-cli-hip-metal-backend-selectors.md) — CLI flags for
  the software-input Metal path; the filter exposes the same
  `metal_device` / `--backend metal` story for hwdec input.
- `ffmpeg-patches/0006-libvmaf-add-libvmaf-vulkan-filter.patch` —
  template the patch 0013 dispatch body mirrors.
- Source: `req` (user direction: "create t8-1d+patch etc..." —
  paraphrased; user clarified mid-session that the intent was the
  IOSurface filter, not the already-merged dispatch-files PR).
- Pairs with: `ffmpeg-patches/0013-libvmaf-add-libvmaf-metal-filter.patch`.
