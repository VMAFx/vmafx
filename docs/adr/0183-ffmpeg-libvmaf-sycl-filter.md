# ADR-0183: `libvmaf_sycl` FFmpeg filter — zero-copy QSV / VAAPI import

- **Status**: Accepted
- **Date**: 2026-04-26
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: sycl, ffmpeg, fork-local, zero-copy

## Context

PR #126 documented a real ergonomic gap in the FFmpeg path: when
users decode video via `-hwaccel vaapi -hwaccel_output_format
vaapi` (or `-hwaccel qsv -hwaccel_output_format qsv` on Intel),
the regular `libvmaf` filter forces a `hwdownload,format=yuv420p`
round-trip — GPU decode, CPU readback, GPU re-upload — which
negates most of the hwdec win. The fork already had the
zero-copy import primitives at the C-API level
([`libvmaf_sycl.h`](../../core/include/libvmaf/libvmaf_sycl.h)
exposes `vmaf_sycl_import_va_surface` and
`vmaf_sycl_dmabuf_import`), but they were not plumbed through
FFmpeg. T7-28 captured this gap.

In parallel, a working local FFmpeg checkout had already
implemented a dedicated `libvmaf_sycl` filter that consumes
oneVPL `mfxFrameSurface1` frames (`AVFrame->data[3]`), extracts
the underlying VA surface ID, and routes through
`vmaf_sycl_import_va_surface` for zero-copy DMA-BUF import. The
work was done; it just had not been packaged as a committed
patch in `ffmpeg-patches/`.

## Decision

We add `ffmpeg-patches/0005-libvmaf-add-libvmaf-sycl-filter.patch`
as the canonical packaging of the local libvmaf_sycl filter.
Configuration:

- New `--enable-libvmaf-sycl` configure switch (separate from
  `--enable-libvmaf`); requires libvmaf ≥ 2.0.0 with SYCL enabled
  (`pkg-config libvmaf_sycl`) plus libvpl for the oneVPL surface
  handle.
- New `ff_vf_libvmaf_sycl` filter registered in
  `libavfilter/allfilters.c` and `libavfilter/Makefile`
  (`CONFIG_LIBVMAF_SYCL_FILTER`).
- The filter shares the existing `LIBVMAFContext` struct with
  `libvmaf` / `libvmaf_cuda`; it adds `sycl_state` + `va_display`
  `sycl_gpu_profile` fields and a dedicated frame callback
  `do_vmaf_sycl()` that imports each frame's VA surface
  zero-copy.
- Pairs with the existing `0003-libvmaf-wire-sycl-backend-selector.patch`
  (which adds a `sycl_device=N` option to the regular `libvmaf`
  filter for the software-frame path); together the two patches
  give users:
  - `libvmaf=sycl_device=N` — software frames + SYCL compute.
  - `libvmaf_sycl=...` — QSV/VAAPI hwdec + zero-copy SYCL.

This closes T7-28. **T7-29 (Vulkan zero-copy)** remains open
because there is no `vmaf_vulkan_import_image` C-API surface
yet — that's an L-sized follow-up needing new
`VkExternalMemoryImageCreateInfo` / `VkSemaphore` plumbing in
[`libvmaf_vulkan.h`](../../core/include/libvmaf/libvmaf_vulkan.h).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Extend `libvmaf` filter (no new filter; detect VAAPI format inside it) | Single filter for all paths | The hwdec bridge requires platform-specific includes (`mfxstructures.h`, libvpl); leaks oneVPL types into the regular `vf_libvmaf.c`; harder to gate via `--enable-libvmaf-sycl` cleanly | Dedicated filter mirrors the `libvmaf_cuda` precedent (CUDA decode is also a separate filter) and keeps the regular filter clean |
| Wait until libvmaf has a generic `vmaf_picture_import_external` API | One filter, backend-agnostic import | The C-API surface that would make this clean doesn't exist; would require a multi-month design pass and gate the user-visible win behind it | Ship the working zero-copy path now; revisit unification later when more backends have import primitives |
| Use VAAPI directly (skip QSV/oneVPL wrapping) | One step shorter; fewer dependencies | FFmpeg's QSV decoder is the path Intel users take for hwdec; raw VAAPI gives a different `AVFrame->data[3]` shape and would need a parallel implementation | The QSV/oneVPL path covers the common Intel case; the plain-VAAPI bridge stays available via `hwdownload,format=yuv420p` + `libvmaf=sycl_device=N` |

## Consequences

- **Positive**: end-to-end QSV decode → SYCL compute now stays
  on the GPU. Removes the slow `hwdownload` round-trip for the
  common Intel hwdec path. User docs in
  [`docs/usage/ffmpeg.md`](../usage/ffmpeg.md) updated to point
  at the new filter; T7-28 backlog row marked closed.
- **Negative**: the patch series has another file to maintain
  on every FFmpeg version bump. The 0005 patch carries the bulk
  of the libvmaf_sycl filter (~280 LOC); rebasing onto a new
  FFmpeg release will surface conflicts here first.
- **Neutral / follow-ups**: T7-29 (Vulkan zero-copy) still open
  in the backlog. Plain VAAPI (non-QSV) hwdec users continue
  to use the `hwdownload` bridge; a future `vf_libvmaf_vaapi`
  could close that subset if demand is real.

## References

- Source: T7-27 PR #126 review surfaced the gap in user docs;
  this ADR closes T7-28.
- Backlog rows: T7-28 (this ADR — closed),
  T7-29 (Vulkan VkImage import — still open) in
  [`.workingdir2/BACKLOG.md`](../../.workingdir2/BACKLOG.md).
- Pairs with: [`ffmpeg-patches/0003-libvmaf-wire-sycl-backend-selector.patch`](../../ffmpeg-patches/0003-libvmaf-wire-sycl-backend-selector.patch)
  (sycl_device option on the regular libvmaf filter).
- Companion C-API: [`core/include/libvmaf/libvmaf_sycl.h`](../../core/include/libvmaf/libvmaf_sycl.h)
  — `vmaf_sycl_import_va_surface`, `vmaf_sycl_dmabuf_import`,
  `vmaf_sycl_wait_compute`.
