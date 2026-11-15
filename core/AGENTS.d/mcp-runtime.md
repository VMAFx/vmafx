---
paths:
  - core/src/mcp/mcp.c
  - core/src/mcp/mcp_internal.h
invariant: Embedded MCP runtime conforms to ADR-0209 transport contract and lifecycle.
area: engine-and-runtime
---
<!-- markdownlint-disable MD013 MD060 -->
# Embedded MCP runtime contract and transport lifecycle

- **Embedded MCP runtime contract** (fork-local, [ADR-0209](../../docs/adr/0209-mcp-embedded-scaffold.md)).
  [`src/mcp/`](../src/mcp/) now contains promoted in-process MCP
  runtime declared in
  [`include/libvmaf/libvmaf_mcp.h`](../include/libvmaf/libvmaf_mcp.h):
  stdio, UDS, and loopback-SSE transports, plus read-only
  `list_features` and out-of-band `compute_vmaf`. Preserve
  early argument validation (`-EINVAL` on NULLs / negative fds /
  NULL paths) before any runtime work; smoke tests for `_init`,
  `_start_uds`, `_start_stdio`, and `_start_sse` rely on that
  contract. `compute_vmaf` must keep using per-call ephemeral
  `VmafContext`, not host scorer, because pooled scoring commits
  models destructively. `enable_mcp` umbrella flag must default
  `false` until mutating measurement-thread tools and SPSC bridge
  land; silent-flip risk is same as ADR-0175's Vulkan
  precedent.
