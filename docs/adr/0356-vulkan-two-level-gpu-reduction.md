# ADR-0356: Two-level GPU reduction for Vulkan VIF / ADM / motion accumulators

- **Status**: Accepted
- **Date**: 2026-05-09
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: `vulkan`, `perf`, `gpu`

## Context

The Vulkan VIF, ADM, and motion kernels accumulate per-metric statistics
using a per-workgroup (WG) int64 SSBO layout: each compute dispatch writes
one slot of `[N_workgroups][N_fields] int64_t`. After the dispatch completes
the CPU reads every slot and sums them. At 1080p the per-WG WG counts are:

- **VIF scale 0**: `ceil(1920/32) × ceil(1080/4)` = 60 × 270 = 16,200 WGs,
  7 fields each = 907,200 bytes (≈ 886 KB) per scale, × 4 scales = **3.5 MB**.
- **ADM stage 3**: `3 × num_active_rows` per scale, × 4 scales = comparable.
- **motion**: 16,200 WGs × 1 field = **129,600 bytes** (≈ 127 KB).

Combined, `reduce_and_emit` / `reduce_sad_partials` in the hot path reads
approximately **1.2 MB** per frame from a host-visible Vulkan buffer. On
discrete GPU (RTX 4090, Arc A380) this region sits in BAR-mapped device-local
or cached host-visible memory and is read uncached over PCIe. A perf-hunt
profiling session found that this host readback accounts for **59.73% of
CPU self-time** for VIF, **5.53%** for ADM, and **6.08%** for motion at 1080p
on RTX 4090 (driver 595.71.05).

The fix is a textbook two-level reduction: add a second compute dispatch per
kernel that reads the N per-WG slots on-GPU and writes a single reduced
result struct to a tiny output buffer. The CPU then reads only the tiny struct
(56 bytes for VIF, 48 bytes for ADM, 8 bytes for motion = **~112 bytes total**
per frame) instead of the full per-WG array.

Integer int64 addition is commutative and associative in two's-complement
arithmetic; any evaluation order produces the same result. Bit-exactness with
the previous host-loop path is therefore guaranteed by construction for all
three kernels.

`VK_EXT_shader_atomic_int64` is required for the reduction shaders (needed for
`atomicAdd` on `int64_t` in GLSL). Its availability on the three target
drivers is verified at device selection time (common.c).

## Decision

We will add three reduction compute shaders (`vif_reduce.comp`,
`adm_reduce.comp`, `motion_reduce.comp`) and companion pipeline objects in
the three host-side TUs. Each reduction shader is dispatched once per
scale/kernel immediately after the per-WG dispatch, in the same command
buffer, separated by a `VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT` memory barrier
on the accumulator SSBO. The host reads only the tiny output buffer after the
fence signals. The per-WG accumulator buffer is retained (the per-WG kernel
still writes it; only the host-side loop is removed).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Host-side AVX2 reduction | Simpler, no new Vulkan extension required | Does not eliminate the PCIe readback — data still crosses BAR uncached before AVX2 touches it. Bandwidth saving is zero. | PCIe bandwidth is the bottleneck, not host compute. |
| Expand per-WG kernel to a single-pass full-frame reduction using atomic_int64 | Eliminates the intermediate per-WG array entirely | `VK_KHR_shader_atomic_int64` is optional and unavailable on some Mesa/lavapipe configs; the fallback path (per-WG array) is what we already have. One dispatch is simpler. | Atomic_int64 on the production path is fine; requiring it only for the tiny reducer (256-thread WGs) is the minimal incremental requirement. |
| Two-level GPU reduction with separate dispatch (chosen) | Eliminates PCIe readback; works with existing per-WG kernel; bit-exact by construction; maps cleanly to the VmafVulkanKernelPipeline template | Adds one pipeline + one dispatch per kernel per frame; requires VK_EXT_shader_atomic_int64 | Best net impact. |
| Async CPU readback + overlap with next frame's upload | Pipelining benefit | Complex sync; fragile with VmafVulkanKernelSubmitPool's single-slot model | Deferred to a future pipelining PR (T-GPU-ASYNC-1). |

## Consequences

- **Positive**: Host-side readback per frame shrinks from ~1.2 MB to ~112
  bytes for VIF + ADM + motion combined. Projected throughput improvement at
  1080p on discrete GPU: +50–80% (compounds with bottleneck #1's
  `HOST_ACCESS_RANDOM_BIT` readback flag fix).
- **Positive**: The tiny output buffer benefits directly from the
  `HOST_ACCESS_RANDOM_BIT` VMA flag (bottleneck #1's fix) because it is
  the only host-read buffer post-reduction.
- **Negative**: One additional `VkPipeline` and one descriptor pool per
  feature kernel at init time (~3 ms amortised over the full run).
- **Negative**: `VK_EXT_shader_atomic_int64` becomes a required extension.
  It is advertised by all three target driver stacks (NVIDIA 595.71.05,
  Mesa anv 24.x for Arc A380, RADV 24.x for RDNA iGPU). Devices without
  it receive a runtime error from `vmaf_vulkan_context_new` (checked via
  `VkPhysicalDeviceShaderAtomicInt64Features::shaderBufferInt64Atomics`).
- **Negative — macOS / MoltenVK**: `shaderBufferInt64Atomics` is **not
  supported** on MoltenVK 1.2.x because Metal does not expose 64-bit
  buffer atomics. On Apple Silicon the device-feature query at backend
  init time will return `VK_FALSE`, `vmaf_vulkan_context_new` will
  return `-ENOTSUP` with a clear stderr message ("Vulkan backend
  disabled on this device — no shaderBufferInt64Atomics support"),
  and the framework falls back to CPU. There is no in-tree macOS
  Vulkan CI lane today, so this branch is reasoned-about but not
  exercised; the guard keeps a future macOS user from seeing an
  opaque `vkCreateShaderModule` / `vkCreateComputePipelines`
  failure.
- **Neutral**: The per-WG accumulator SSBO is retained; only the host-loop
  reader is removed. A future PR (T-GPU-PERF-VK-3b) could eliminate the
  per-WG SSBO by folding the reduction into the per-WG kernel using shared
  memory + single global atomicAdd — but that removes the two-pass structure
  that makes bit-exactness independent of WG execution order.
- **Follow-up**: The float-variant kernels (float_vif, float_adm,
  float_motion) have smaller per-WG arrays (float64 instead of int64 fields)
  and were not in the perf-hunt top-3; document them as future-work.

## References

- perf-hunt report (session 2026-05-09): VIF `reduce_and_emit` 59.73% CPU
  self-time, ADM 5.53%, motion 6.08% on RTX 4090 at 1080p.
- `req`: "replace the 1.2 MB-per-frame host readback of per-workgroup
  accumulators with a two-level GPU reduction that emits a single ~56-byte
  struct per scale per frame."
- [ADR-0175](0175-vulkan-backend-scaffold.md) — Vulkan backend scaffold.
- [ADR-0246](0246-vulkan-kernel-template.md) — VmafVulkanKernelPipeline template.
- [ADR-0269](0269-vif-shared-memory-race.md) — memoryBarrierShared / barrier
  pattern for the reduction scratch memory.
- Vulkan 1.3 spec §7.1 "Execution and Memory Dependencies",
  §7.5.3 "VkMemoryBarrier" — the pipeline barrier between the per-WG
  dispatch and the reducer dispatch.
- `VK_EXT_shader_atomic_int64` extension spec — `shaderBufferInt64Atomics`
  capability required for `atomicAdd` on `int64_t` SSBOs.
