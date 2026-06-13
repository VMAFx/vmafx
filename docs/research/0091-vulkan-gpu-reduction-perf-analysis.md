<!-- markdownlint-disable MD013 MD060 -->
# Research digest 0091 — Vulkan per-WG accumulator readback bottleneck and two-level GPU reduction

**Date**: 2026-05-09
**Authors**: Lusoris / Claude (Anthropic)
**Related ADR**: [ADR-0356](../adr/0356-vulkan-two-level-gpu-reduction.md)

## Problem statement

A perf-hunt session on the Vulkan backend at 1080p (RTX 4090, driver 595.71.05)
identified that the host-side reduction loop reading per-workgroup int64
accumulator slots from a host-visible Vulkan buffer dominated CPU self-time:

| Kernel | Function | CPU self-time share |
|---|---|---|
| VIF | `reduce_and_emit` (vif_vulkan.c:465–535) | **59.73%** |
| motion | `reduce_sad_partials` (motion_vulkan.c:447–454) | **6.08%** |
| ADM | `reduce_and_emit` (adm_vulkan.c:721–798) | **5.53%** |
| All others | — | < 3% each |

## Root cause

At 1080p with VIF workgroup geometry `WG_X=32, WG_Y=4`:

- Scale 0: `gx = ceil(1920/32) = 60`, `gy = ceil(1080/4) = 270`, `wg_count = 16,200`.
- The per-WG accumulator buffer size: `16,200 × 7 fields × 8 bytes = 907,200 bytes ≈ 886 KB`.
- For 4 scales (WG counts: 16200, 4050, 1013, 254): total accumulator read = ~1.2 MB/frame.

On discrete GPU this region is BAR-mapped device-local or cached host-visible.
PCIe BAR reads are uncached and incur full PCIe round-trip latency per cache
line (~64 B). At 1.2 MB and PCIe Gen 4 ×16 bandwidth (~26 GB/s read BAR),
the raw bandwidth cost alone is ~46 µs per frame; with read-request overhead
(PCIe request granularity, TLP overhead) the actual measured time was closer
to 2–3 ms per frame — enough to dominate the 5–8 ms/frame budget at 200 fps.

For motion, `wg_count = 16,200`, 1 field × 8 bytes = 129,600 bytes. For ADM,
`3 × active_rows` per scale × 6 fields × 8 bytes × 4 scales ≈ 1.1 MB.

## Solution: two-level GPU reduction

Replace the host loop with a second compute dispatch that runs on-GPU:

1. The per-WG kernel still writes its slot into the large accumulator SSBO
   (no shader change to the hot path).
2. A new tiny reducer shader (256 threads/WG, grid-stride loop over all slots)
   reads the per-WG SSBO, does subgroup-add + shared-memory cross-subgroup
   reduction, then writes a single `atomicAdd` per field into a tiny
   output buffer (`N_fields × sizeof(int64_t)`).
3. The CPU reads only the tiny output buffer (56 B for VIF, 48 B for ADM,
   8 B for motion = **112 bytes total** across all three features per frame).

### Before/after readback bytes per frame (1080p, VIF + ADM + motion)

| | VIF | ADM | motion | Total |
|---|---|---|---|---|
| Before | ~1.2 MB | ~1.1 MB | ~130 KB | ~2.4 MB |
| After | 224 B (4 × 56 B) | 192 B (4 × 48 B) | 8 B (1 × 8 B) | **424 B** |

Reduction factor: **~5,800×** fewer bytes read by the CPU from GPU-visible
memory per frame.

### Bit-exactness guarantee

int64 (two's-complement 64-bit signed integer) addition is both commutative
and associative. A tree-reduction of N integers produces the same result as a
sequential loop for any evaluation order. No floating-point operations are
involved in the accumulator buffers or the reduction shaders. The final int64
totals read by the CPU are mathematically identical to the previous host loop.

This is NOT true for floating-point (FMA contraction, operand-order sensitivity
at the ULP level). The VIF / ADM / motion accumulators were deliberately kept
as int64 for this reason (see vif.comp §"int64 representation").

### Projected throughput impact

Removing ~2.4 MB of PCIe BAR reads per frame at the 59% CPU-bottleneck position
is expected to increase VIF-only throughput from ~130 fps to ~200 fps on RTX 4090
(+54%), compounding with bottleneck #1's `HOST_ACCESS_RANDOM_BIT` flag fix which
already improved staging-buffer reads. The combined improvement target is ≥ 2×
over the 100 fps pre-fix baseline.

### Extension requirements

`VK_EXT_shader_atomic_int64` (`shaderBufferInt64Atomics` feature) is required
for `atomicAdd` on SSBO `int64_t` in the reducer shaders. Availability on
target drivers:

| Driver | Advertised |
|---|---|
| NVIDIA 595.71.05 (RTX 4090) | Yes (`VkPhysicalDeviceShaderAtomicInt64Features`) |
| Mesa anv 24.x (Arc A380) | Yes |
| RADV 24.x (RDNA iGPU, lavapipe) | Yes |
| lavapipe (CI) | Yes (Mesa 24.x) |

### Memory barrier correctness

The reducer dispatch is recorded in the same command buffer immediately after
the per-WG dispatch, separated by a `VkMemoryBarrier`:

```text
srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT
dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
srcStageMask  = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
dstStageMask  = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
```

This satisfies Vulkan 1.3 spec §7.1 "Execution and Memory Dependencies" for
write-after-write (the reducer also writes `reduced_accum` via atomicAdd)
and read-after-write ordering. No `vkQueueWaitIdle` between dispatches.

The CPU reads `reduced_accum` only after `vkWaitForFences` (which the
existing `vmaf_vulkan_kernel_submit_end_and_wait` call provides). After the
fence signals, `vmaf_vulkan_buffer_invalidate` is called to flush any
non-coherent host cache before `vmaf_vulkan_buffer_host` reads the result.

### Per-reducer-WG reduction in the shader

The reduction shader uses the pattern already established in `vif.comp`
Phase 4 (see research-0089 / ADR-0269):

- subgroupAdd for the per-subgroup partial;
- elected threads write subgroup partials to shared memory;
- `memoryBarrierShared(); barrier();` before thread 0 reads;
- thread 0 sums subgroup partials and calls `atomicAdd` into the output SSBO.

The `memoryBarrierShared(); barrier();` pair is mandatory for correctness on
NVIDIA Vulkan 1.4 (driver 595.71.05) where the stricter default memory model
reorders the elected-thread writes past thread-0's reads without it. See
research-0089 for the empirical race evidence.

## Future work

- **Float-variant kernels** (float_vif, float_adm, float_motion): these use
  per-WG float64 accumulators. The readback savings are smaller (~200 KB vs
  ~2.4 MB for integer variants) but the same pattern applies. Deferred.
- **Fold reducer into per-WG kernel** (T-GPU-PERF-VK-3b): eliminate the
  intermediate per-WG SSBO by using one global `atomicAdd` per field per WG.
  This saves the second dispatch overhead but requires the per-WG kernel to
  be restructured. Deferred.
- **Pipeline pipelining** (T-GPU-ASYNC-1): overlap the reducer dispatch for
  frame N with the per-WG upload for frame N+1 via timeline semaphores.
