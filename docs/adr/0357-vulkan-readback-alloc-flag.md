<!-- markdownlint-disable MD060 -->
# ADR-0357 — Vulkan readback buffer VMA allocation flag separation

| Field    | Value                                      |
|----------|--------------------------------------------|
| Status   | Accepted                                   |
| Date     | 2026-05-09                                 |
| Deciders | lusoris                                    |
| Area     | vulkan, performance                        |

## Context

The Vulkan backend allocates every host-mapped buffer with a single call to
`vmaf_vulkan_buffer_alloc`, which passes
`VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT` to VMA.  This flag
tells VMA to prefer a **write-combining / BAR heap** on discrete GPUs — the
ideal choice for upload traffic (CPU writes, GPU reads) because write-combining
coalesces CPU stores efficiently before they cross PCIe.

However, the accumulator and partial-sum buffers are written by the GPU and
read back by the CPU for the final reduction step.  On a write-combining BAR
heap, CPU reads are uncached and require PCIe round-trips per cache line,
giving 4–8× worse bandwidth than a cached host read (measured on AMD RDNA3:
~6 GB/s vs ~40 GB/s).  These readback buffers are typically small (a few KB
per feature per frame), so the absolute data volume is low, but the latency of
uncached reads dominates the post-fence reduction loop.

The fix is to allocate readback buffers with
`VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT`, which causes VMA to prefer a
`HOST_CACHED` heap on discrete GPUs (VMA §5.3).  CPU reads become cached
DRAM bandwidth.  A matching `vmaInvalidateAllocation` call (wrapped as
`vmaf_vulkan_buffer_invalidate`) is required before each CPU readback on
non-coherent heaps (Vulkan 1.3 spec §11.2.2); VMA makes it a no-op on
HOST\_COHERENT heaps (e.g., integrated GPUs), so the call is unconditionally
safe.

Profiling baseline: ~30–50% throughput increase at 1080p on dGPU expected,
based on the read-bandwidth improvement and the relative weight of the
post-dispatch reduction in the per-frame budget.

## Decision

Split `vmaf_vulkan_buffer_alloc` into two sibling functions in
`core/src/vulkan/picture_vulkan.{c,h}`:

- `vmaf_vulkan_buffer_alloc()` — UPLOAD buffers (CPU writes, GPU reads).
  Unchanged VMA flag: `HOST_ACCESS_SEQUENTIAL_WRITE | MAPPED`.
- `vmaf_vulkan_buffer_alloc_readback()` — READBACK buffers (GPU writes,
  CPU reads).  VMA flag: `HOST_ACCESS_RANDOM | MAPPED`.

Add `vmaf_vulkan_buffer_invalidate()` wrapping `vmaInvalidateAllocation`,
to be called after GPU fence-wait before every CPU read from a readback
buffer.

Audit all 17 feature kernel files and switch accumulator/partial-sum buffer
allocations to `alloc_readback`, adding the corresponding `invalidate` calls
immediately before the CPU-reduction loops.

### Buffer classification (by feature)

| Feature file            | Readback buffer(s)                      |
|-------------------------|-----------------------------------------|
| `vif_vulkan.c`          | `scale[].accum`                         |
| `adm_vulkan.c`          | `accum[scale]`                          |
| `motion_vulkan.c`       | `sad_partials`                          |
| `motion_v2_vulkan.c`    | `sad_partials`                          |
| `ssim_vulkan.c`         | `partials`                              |
| `ms_ssim_vulkan.c`      | `l_partials`, `c_partials`, `s_partials`|
| `psnr_vulkan.c`         | `se_partials[p]`                        |
| `ciede_vulkan.c`        | `partials`                              |
| `psnr_hvs_vulkan.c`     | `partials[p]`                           |
| `float_psnr_vulkan.c`   | `partials`                              |
| `float_vif_vulkan.c`    | `num_partials[i]`, `den_partials[i]`    |
| `float_adm_vulkan.c`    | `accum[scale]`                          |
| `float_motion_vulkan.c` | `sad_partials`                          |
| `float_ansnr_vulkan.c`  | `sig_partials`, `noise_partials`        |
| `moment_vulkan.c`       | `sums`                                  |
| `ssimulacra2_vulkan.c`  | `mu1`, `mu2`, `s11`, `s22`, `s12`      |
| `cambi_vulkan.c`        | `image_buf`, `mask_buf`, `scratch_buf`  |

UPLOAD buffers (CPU writes → flush → GPU reads) are left unchanged.  Buffers
that are bidirectional but managed by VMA `HOST_ACCESS_RANDOM` (cambi's image/
mask/scratch) still support both `vmaFlushAllocation` (CPU→device) and
`vmaInvalidateAllocation` (device→CPU); the flush calls in cambi remain valid.

## Alternatives considered

| Option | Description | Reason rejected |
|--------|-------------|-----------------|
| **A: VMA flag parameter** | Add a `readback` bool to `vmaf_vulkan_buffer_alloc` | Adds a boolean trap to a widely-called function; callers must understand the flag semantics. Sibling function gives a meaningful name at the call site. |
| **B: Auto-detect by usage** | Inspect `VK_BUFFER_USAGE_*` bits to pick the flag automatically | No single `VkBufferUsageFlagBits` maps cleanly to "CPU reads the result". All readback buffers also carry `STORAGE` + `TRANSFER_DST` which upload buffers also use. |
| **C: Device-local + staging copy** | Allocate accumulators device-local; copy to host staging via `vkCmdCopyBuffer` per frame | Adds a staging buffer per feature, a copy dispatch, and an extra submission. For small (few-KB) accumulator buffers the DMA overhead exceeds the cache benefit. VMA `HOST_ACCESS_RANDOM` with HOST_CACHED gives the same cache performance with zero extra infrastructure. |
| **D: No change** | Leave all buffers on the BAR / write-combining heap | Measured 4–8× CPU readback penalty on AMD dGPU; this is bottleneck #1 in the Vulkan perf hunt. |

## Consequences

- CPU readback from accumulator and partial-sum buffers uses host-cache
  bandwidth on discrete GPUs, eliminating the primary post-fence CPU stall.
- `vmaf_vulkan_buffer_flush` is now **only** called on UPLOAD buffers (and
  on cambi's bidirectional buffers).  Calling flush on a readback buffer is
  not wrong (VMA handles it), but is unnecessary and confusing — a follow-on
  lint rule can enforce this.
- `vmaf_vulkan_buffer_invalidate` must be called after every fence-wait before
  reading a readback buffer.  This invariant is documented in `picture_vulkan.h`
  and `core/src/vulkan/AGENTS.md`.
- No change to the SPIR-V shaders, descriptor layouts, or pipeline caches.
- No change to the public libvmaf API or ffmpeg-patches surface.

## References

- req: "fix Vulkan VMAF performance bottleneck #1 — VMA allocation flag
  causing 4–8× slower CPU readback on discrete GPU"
- VMA §5.3 — Memory usage, HOST_ACCESS_SEQUENTIAL_WRITE vs HOST_ACCESS_RANDOM
- Vulkan 1.3 spec §11.2.2 — Host access to device memory, non-coherent heaps
- [ADR-0175](0175-vulkan-backend-scaffold.md) — Vulkan backend scaffold
- [ADR-0186](0186-vulkan-image-import-impl.md) — Vulkan image-import contract
