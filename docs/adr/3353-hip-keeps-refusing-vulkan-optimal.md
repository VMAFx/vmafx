<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3353: HIP devices keep refusing Vulkan images with OPTIMAL tiling

- **Status**: Accepted
- **Date**: 2026-10-18
- **Deciders**: Lusoris (operator order vmafx-next-20261018-1650, paraphrased: take the next open 0.1.0 row)
- **Tags**: api, rc4, gpu, hip, vulkan, zero-copy

## Context

[ADR-2152](2152-vmafx-vulkan-frame-import.md) imports Vulkan frames on
CUDA, SYCL and HIP. CUDA reads OPTIMAL images as CUDA arrays. SYCL and HIP
refuse them and name `desc.vulkan_tiling`. The state row
`T-VULKAN-IMPORT-OPTIMAL-CUDA-ONLY-2026-10-06` stays open until one of two
things happens: OPTIMAL imports on HIP become bit-exact over repeated runs,
or an ADR records that HIP keeps refusing them.

HIP does offer the CUDA-style route: `hipImportExternalMemory()` of the
image's OPAQUE_FD memory, then `hipExternalMemoryGetMappedMipmappedArray()`
and `hipMipmappedArrayGetLevel()` for each plane. On 2026-10-18 that route
was implemented and run against `test_vmafx_import_vulkan_hip`. The device
was a gfx1036 GPU, the producer RADV (Mesa 26.2), and the frames 576x324
YUV420P, one disjoint OPTIMAL image per plane. The runtime was the ROCm
runtime libraries of the pinned dev image (HIP 7.02, ROCm 10.1.0 image)
with kernels from the host's hipcc 7.2.4. That is a stated substitution:
the pinned image has no Vulkan driver to produce the frames.

- On host ROCm 7.2.4 the runtime does not export
  `hipExternalMemoryGetMappedMipmappedArray()`, so `hipGetProcAddress()`
  is the only way to reach it.
- On the pinned runtime every call returned `hipSuccess`. The first
  `hipMemcpy2DFromArrayAsync()` out of the luma array then stopped the
  process with a GPU memory fault in `__amd_rocclr_copyImageToBuffer`.
- The runtime's trace shows the cause. RADV's luma allocation is 0x36000
  bytes. The image object ROCclr builds over it spans 0x2d900 bytes, which
  is exactly 576 x 324: ROCclr describes the memory as a linear image.
  RADV laid the image out in its own swizzled tiling, padded to 0x36000.
  No HIP descriptor carries the producer's tiling or modifier.
- Passing the plane size and the dedicated-allocation flag, as the CUDA
  import does, did not change the result.

The earlier probe in ADR-2152's research,
[Research-2162](../research/2162-vmafx-vulkan-frame-import.md), read 576x324
frames back identical in 4 of 5 runs and returned a different P010 chroma
plane in the fifth. Even where it does not fault, the interpretation of
the layout is not reliable.

## Decision

HIP devices keep refusing Vulkan images with OPTIMAL tiling, naming
`desc.vulkan_tiling`, as they do today. A producer that scores on a HIP
device exports buffers, LINEAR images or linear DRM-modifier images; these
go through the dma-buf route of ADR-2152. The state row closes with this
record.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep the refusal (chosen) | No wrong scores; nothing new to maintain; the producer-side copy to LINEAR is already documented | A producer that holds OPTIMAL frames copies them once on its GPU | — |
| Map OPTIMAL memory as HIP arrays (CUDA's route) | No producer-side copy | Faults on the pinned runtime; ROCclr describes the memory as linear while RADV tiles it, so the bits cannot be the producer's; the entry point is missing from host ROCm 7.2.4 | Not bit-exact, and not even safe to run |
| Read the tiling through a DRM modifier and de-tile on the device | Would work for any layout the modifier names | OPTIMAL tiling has no modifier by definition; with a modifier the image is DRM_FORMAT_MODIFIER tiling, which the dma-buf route already handles | The case it solves is not OPTIMAL |

## Consequences

- **Positive**: a HIP score never rests on a guess about a driver-private
  layout. The refusal and its test (`test_vmafx_import_vulkan_hip`,
  `test_refusals_named`) are unchanged.
- **Negative**: producers on AMD GPUs copy OPTIMAL frames to a LINEAR image
  or buffer before the import, as the
  [Vulkan frames](../api/vmafx/index.md#vulkan-frames) guide already says.
- **Neutral / follow-ups**: revisit when a ROCm release documents image
  interop with Mesa's Vulkan driver, or when ROCm can take the producer's
  tiling for an imported image. The measurement is then rerun on
  `test_vmafx_import_vulkan_hip_bitexact` (4K and P016 included) over
  repeated runs.

## References

- [ADR-2152](2152-vmafx-vulkan-frame-import.md), [Research-2162](../research/2162-vmafx-vulkan-frame-import.md).
- State row `T-VULKAN-IMPORT-OPTIMAL-CUDA-ONLY-2026-10-06` in [state](../state.md).
- HIP runtime API: `hipExternalMemoryGetMappedMipmappedArray()`, `hipMipmappedArrayGetLevel()` (ROCm HIP reference).
- Source: operator order vmafx-next-20261018-1650 (paraphrased).
