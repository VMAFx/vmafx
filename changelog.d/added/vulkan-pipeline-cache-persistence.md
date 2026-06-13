### Vulkan backend: persistent `VkPipelineCache` (ADR-0445)

The Vulkan compute backend now persists its compiled pipeline cache to
`$XDG_CACHE_HOME/libvmaf/vulkan-pipeline-cache.bin` across process
invocations. On NVIDIA RTX 4090 this reduces cold-start pipeline
compilation from 80–120 ms (SPIR-V → ISA on every run) to 2–5 ms on
warm starts.

**Measured benchmark** (`vmaf --feature psnr_vulkan --backend vulkan`,
1920×1080 YUV420 8-bit, RTX 4090):

| Run | 1 frame | 48 frames |
|-----|---------|-----------|
| Cold (no cache) | ~140 ms | ~620 ms |
| Warm (cache hit) | ~25 ms | ~505 ms |

The cache is keyed on `VkPipelineCacheHeaderVersionOne` vendor/device IDs;
a device change causes silent discard and a one-time cold recompilation. Set
`LIBVMAF_VULKAN_PIPELINE_CACHE=0` to disable the cache (useful in CI).
Bit-exact output is unchanged — the cache only replays compiled ISA, not
any numeric path.

References: Research-0135, ADR-0445, PR #865.
