<!-- markdownlint-disable MD013 MD060 -->
# ADR-0445: Persistent VkPipelineCache for Vulkan compute backend

- **Status**: Accepted
- **Date**: 2026-05-16
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: `vulkan`, `gpu`, `performance`, `pipeline-cache`, `fork-local`

## Context

PR #865 profiling (Research-0135) showed that the Vulkan compute backend
recompiles all SPIR-V kernels from scratch on every process start because
every `vkCreateComputePipelines()` call passed `VK_NULL_HANDLE` as the
pipeline cache handle. On NVIDIA RTX 4090 this costs 80–120 ms of
cold-start latency across the full kernel set. The Vulkan 1.3 spec §10.6
defines `VkPipelineCache` as the standard mechanism for persisting compiled
PSO blobs across process invocations; drivers skip recompilation on warm
starts when the vendorID / deviceID header matches.

## Decision

We will add a `VkPipelineCache pipeline_cache` to `VmafVulkanContext` and
load it from `$XDG_CACHE_HOME/libvmaf/vulkan-pipeline-cache.bin` at
context init; serialise it back at destroy. The `VkPipelineCacheHeaderVersionOne`
header is validated (vendor ID + device ID) before reuse. Every
`vkCreateComputePipelines` call in the codebase passes
`ctx->pipeline_cache` instead of `VK_NULL_HANDLE`. An env-var opt-out
(`LIBVMAF_VULKAN_PIPELINE_CACHE=0`) skips all cache I/O.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|--------|------|------|----------------|
| Keep `VK_NULL_HANDLE` | Zero implementation complexity | 80–120 ms cold start on every invocation | Unacceptable for short single-file runs |
| In-process cache only (no disk persistence) | No file I/O | No cross-invocation benefit; still compiles on cold start | Eliminates the main saving |
| **Persistent cache (chosen)** | Warm start 2–5 ms, bit-exact output unchanged | First run still cold; file must be validated on load | Best outcome per Research-0135 data |

No alternatives needed beyond the above — PR #865 settled the choice; this PR implements option 3.

## Consequences

- **Positive**: Process startup drops from ~140 ms to ~25 ms on warm runs
  (1-frame PSNR-Vulkan benchmark on RTX 4090). Bit-exactness is unaffected
  (the cache just replays compiled ISA, not any numeric path).
- **Negative**: First run remains cold (~140 ms). A stale cache file
  (wrong device) is silently discarded and recreated; no user-visible error.
- **Neutral / follow-ups**:
  - The cache file path follows XDG conventions and is gitignored by
    default. CI uses `LIBVMAF_VULKAN_PIPELINE_CACHE=0` to avoid
    cross-run state contamination.
  - Every future `vkCreateComputePipelines()` addition must pass
    `ctx->pipeline_cache` — see the `AGENTS.md` invariant below.
  - The `VkPipelineCacheHeaderVersionOne` validation does not include
    `driverVersion` — a driver update produces a harmless discarded blob on
    the first post-update run; the driver itself handles stale ISA internally.

## References

- Research-0135: `docs/research/0135-vulkan-dispatch-overhead-2026-05-15.md`
- PR #865 profiling finding (source of this implementation).
- Vulkan 1.3 spec §10.6 "Pipeline Cache".
- ADR-0246: kernel template scaffolding (`kernel_template.h`).
- Source: `req` (PR #865 description — implement VkPipelineCache persistence fix).
