<!-- markdownlint-disable MD060 -->
# ADR-0470: Disk-Persistent VkPipelineCache for Vulkan Feature Extractors

- **Status**: Accepted
- **Date**: 2026-05-16
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `vulkan`, `perf`, `build`

## Context

Every call to `vkCreateComputePipelines` in `kernel_template.h` passed
`VK_NULL_HANDLE` as the cache argument (audit finding VK-4 from the
2026-05-16 perf audit). With 14+ compute pipelines compiled per full VMAF
run, the aggregate cold-start cost from driver re-linking is 200–700 ms on
NVIDIA hardware (per Vulkan spec guide §10.6 benchmark figures). PR #867
reduced shader-module creation time (140 ms → 25 ms via embedded SPIR-V),
but pipeline linking — the driver converting SPIR-V to native ISA — was
still repeated on every process start.

`VkPipelineCache` is the standard Vulkan mechanism for persisting the
result of pipeline compilation to a driver-specific binary blob so
subsequent process launches can skip re-linking entirely.

## Decision

We will create a `VkPipelineCache` per `VmafVulkanContext`, keyed by the
physical device UUID, stored at:

```text
${XDG_CACHE_HOME:-$HOME/.cache}/vmaf/vulkan/<device-uuid>.bin
```

The cache is loaded in `vmaf_vulkan_context_new()` after the device is
created, passed to every `vkCreateComputePipelines` call via
`ctx->pipeline_cache` in `kernel_template.h`, and serialised back to disk
in `vmaf_vulkan_context_destroy()` before the device is torn down.

Failures at any stage (missing home dir, read-only filesystem,
`vkCreatePipelineCache` error) are silently tolerated: `ctx->pipeline_cache`
remains `VK_NULL_HANDLE` and the callers fall through to uncached pipeline
creation — identical behaviour to the pre-patch state.

The external-handle path (`vmaf_vulkan_context_new_external`, used by
FFmpeg's `vf_libvmaf_vulkan`) does not load or save the cache; the caller
owns the device lifecycle and may have its own caching policy.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| In-memory-only cache (no persistence) | Simpler, no filesystem I/O | No cross-process benefit; multi-feature runs within one process already see no re-link | Doesn't address the cold-start cost that motivated the finding |
| Per-feature-extractor cache files | Finer invalidation granularity | 14+ files instead of one; same device UUID keys them all anyway | Unnecessary complexity; the driver validates the blob internally |
| System-level shared cache (e.g. `/var/cache/`) | Shared across users | Requires elevated permissions to write | Not appropriate for a user-space library |

## Consequences

- **Positive**: Warm-start pipeline creation skips driver ISA compilation.
  Expected saving: 200–700 ms per full multi-feature Vulkan run (varies by
  driver and feature count). First cold run is unchanged.
- **Positive**: The fix is transparent — no public API changes, no new CLI
  flags, no new build options.
- **Negative**: First write adds a small filesystem I/O at process exit
  (~50–500 KiB blob, typically under 5 ms). Subsequent runs pay only a
  read at init.
- **Neutral**: The driver validates the cache blob header (device UUID,
  driver version, Vulkan version). On driver upgrades the blob is silently
  rejected and a fresh cache is created — no stale-cache risk.
- **Neutral / follow-up**: The external-handle path could also benefit;
  deferred to a separate ABI-bump PR.

## References

- Vulkan spec §10.6 — Pipeline Cache Objects
- Perf audit finding VK-4: `.workingdir/perf-audit-vulkan-sycl-2026-05-16.md`
- PR #867 — SPIR-V embed (prior cold-start fix)
- `core/src/vulkan/common.c` — `pipeline_cache_load` / `pipeline_cache_save_and_destroy`
- `core/src/vulkan/kernel_template.h` — `vmaf_vulkan_kernel_pipeline_init`, `vmaf_vulkan_kernel_pipeline_add_variant`
