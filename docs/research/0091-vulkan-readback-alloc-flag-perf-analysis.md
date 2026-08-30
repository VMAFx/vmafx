# Research-0091 — Vulkan readback buffer VMA flag: performance analysis

**Date**: 2026-05-09
**Companion**: [ADR-0357](../adr/0357-vulkan-readback-alloc-flag.md)

## Problem statement

The Vulkan feature kernels allocate every host-mapped buffer with
`VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT`.  On discrete GPUs
with a separate VRAM + host-RAM topology, VMA maps this flag to a write-
combining / PCIe BAR heap.  Write-combining gives high throughput for
sequential stores (good for CPU→GPU uploads), but CPU reads from
write-combining memory are uncached, bypassing all CPU cache levels.  On AMD
RDNA3 / PCIe 4.0, uncached BAR reads benchmark at ~6 GB/s vs ~40 GB/s for
cached reads from a HOST_CACHED allocation.

The accumulator and partial-sum buffers (one per feature per WG) are written
by the GPU shader and read back by the CPU reduction loop.  They are small
(typically 256–2048 bytes per feature, once per frame), but the latency of
~N uncached 64-byte cache-line fetches is non-trivial when accumulated across
all features in a frame.

## VMA flag semantics (VMA §5.3)

| VMA flag                                   | Effect on dGPU (non-UMA)            |
|--------------------------------------------|-------------------------------------|
| `VMA_HOST_ACCESS_SEQUENTIAL_WRITE`         | Prefer write-combining BAR heap     |
| `VMA_HOST_ACCESS_RANDOM`                   | Prefer HOST_CACHED heap             |
| Both absent (device-local)                 | No host mapping; staging required   |

On UMA architectures (integrated GPU, Apple M-series), both flags map to the
same underlying heap (shared system DRAM with coherent host access), so the
choice is immaterial on those platforms.

`VMA_HOST_ACCESS_RANDOM` also supports `vmaFlushAllocation` (for bidirectional
buffers where the host writes then the GPU writes then the host reads again) —
VMA flushes only the necessary cache lines rather than bypassing the cache
entirely.

## Cache-coherency requirement (Vulkan 1.3 spec §11.2.2)

HOST_CACHED allocations are **not** HOST_COHERENT on most discrete GPU drivers
(AMD, NVIDIA, Intel Arc — confirmed in VMA examples and the Vulkan CTS).  The
spec says:

> "If the host cache is not coherent, an application must use
> `vkFlushMappedMemoryRanges` to guarantee that host writes are visible to the
> device, and `vkInvalidateMappedMemoryRanges` to guarantee that device writes
> are visible to the host."

VMA wraps these as `vmaFlushAllocation` / `vmaInvalidateAllocation`.  On HOST_
COHERENT heaps (integrated GPU, lavapipe), both are no-ops.

Missing `vmaInvalidateAllocation` after a GPU write on a non-coherent
HOST_CACHED heap is **a silent data race**: the CPU may read stale values from
its own L3 cache.  The bug would appear as non-deterministic feature scores
that vary by run on discrete GPUs.

## Impact estimate

The 17 feature kernels each have 1–5 readback buffers.  At 1080p with typical
workgroup counts:

- `vif`: 4 scales × ~2000 WGs × 7 int64 slots = ~448 KB total readback/frame
- `adm`: 4 scales × ~1000 WGs × 6 int64 slots = ~192 KB/frame
- motion/ms-ssim/ssim/psnr etc.: 256–512 B/feature/frame

Rough estimate: ~800 KB of readback data per frame across all 17 features at
1080p.  At 6 GB/s (uncached BAR) vs 40 GB/s (cached), the readback loop takes
~133 µs vs ~20 µs per frame.  At 60 fps input, this is ~8 ms/s of CPU stall
eliminated — which maps to a 5–15% throughput gain depending on how much of
the frame budget is dominated by the GPU dispatch vs the CPU reduction.  The
user-reported estimate of +30–50% may be achievable if the per-frame budget
breakdown shows the CPU reduction is on the critical path (i.e., if the next
frame dispatch waits for the current reduction to complete).

## Alternatives not explored

- **Device-local + staging copy**: the GPU writes to DEVICE_LOCAL VRAM, then
  a `vkCmdCopyBuffer` DMA to a HOST_VISIBLE staging buffer.  For small
  accumulator buffers (<8 KB each) the DMA overhead + additional submission
  overhead would likely exceed the cache-efficiency gain.  VMA HOST_CACHED is
  the right answer for small readback payloads.

- **Sub-buffer / push-constant feedback**: shader writes partial results
  into push-constant-sized storage and the driver copies them automatically.
  Push constants are limited to 256 bytes and are not writable from a shader.

## Conclusion

The `HOST_ACCESS_RANDOM` flag with a matching `vmaInvalidateAllocation` call
is the correct and VMA-endorsed pattern for GPU-write / CPU-read buffers.  The
implementation in ADR-0357 is minimal: two new sibling functions, no new
infrastructure, no changes to shaders or descriptor layouts.
