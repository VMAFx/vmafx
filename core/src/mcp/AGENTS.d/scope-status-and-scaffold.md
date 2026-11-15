---
paths:
  - core/src/mcp/mcp.c
  - core/src/mcp/mcp_internal.h
  - core/src/mcp/meson.build
  - core/include/libvmaf/libvmaf_mcp.h
  - core/test/test_mcp_smoke.c
invariant: Embedded MCP scope, live transport status, early -EINVAL validation, and enable_mcp build flags.
---
<!-- markdownlint-disable MD013 -->
# Embedded MCP scope, status, and runtime contract

## Scope

```text
mcp/
  mcp.c               # public entry points, lifecycle, listener bring-up
  mcp_internal.h      # runtime types shared across TUs
  dispatcher.c        # JSON-RPC routing (tools/list, tools/call, resources/list)
  compute_vmaf.c      # per-call ephemeral VmafContext scoring tool
  transport_stdio.c   # line-delimited JSON-RPC on stdin/stdout
  transport_uds.c     # AF_UNIX listener (mode 0700), one client at a time
  transport_sse.c     # AF_INET loopback HTTP/1.1 + Server-Sent Events
  meson.build         # subdir() include from core/src/meson.build
```

Public C-API: [`../../include/libvmaf/libvmaf_mcp.h`](../../../include/libvmaf/libvmaf_mcp.h).
Smoke test: [`../../test/test_mcp_smoke.c`](../../../test/test_mcp_smoke.c).

## Backend status

**Live** (T5-2b + v2 + v3, [ADR-0209](../../../../docs/adr/0209-mcp-embedded-scaffold.md)).
All three transports = real implementations: stdio,
`AF_UNIX` UDS (mode 0700, one client at a time), and
loopback-only HTTP/1.1 + SSE (fork-owned plain POSIX sockets;
mongoose rejected on license grounds). Every public entry point validates
arguments first (`-EINVAL` on NULLs / negative fds / NULL paths); smoke
test pins both input-validation contract and live transport execution.

## Rebase-sensitive invariants

- **Smoke test pins validation and live transport contract.**
  [`../../test/test_mcp_smoke.c`](../../../test/test_mcp_smoke.c) runs
  18 sub-tests (`k_test_table[]`) covering availability accessors, early
  `-EINVAL` guards on every entry point, `close(NULL)` and `close(&NULL)`
  no-ops, init/close lifecycle, live stdio/UDS/SSE JSON-RPC exchanges, and
  real 8-bit + 10-bit `compute_vmaf` scoring; disabled builds
  (`-Denable_mcp=false`) stay guarded by
  [`../../test/test_vmafx_mcp_absent.c`](../../../test/test_vmafx_mcp_absent.c).
  Keep both early `-EINVAL` guards and live transport assertions green when
  changing MCP surface.
- **`enable_mcp` umbrella flag defaults `false`**. Silent-flip
  risk = same as ADR-0175's Vulkan precedent. Keep opt-in default unless
  superseding ADR changes build defaults.
- **Per-build-flag availability**. Umbrella `enable_mcp` flag
  flips `HAVE_MCP`; per-transport sub-flags flip matching
  `HAVE_MCP_*` macros (`HAVE_MCP_SSE`, `HAVE_MCP_UDS`,
  `HAVE_MCP_STDIO`). Header surface stays identical either way
  (disabled builds return `-ENOSYS` / `VMAFX_E_NOTSUP`). **On rebase**: keep
  per-transport bitmask fold-pattern in `vmaf_mcp_transport_available` —
  preprocessor-fed arithmetic compiles to constant load +
  bittest at every call site, avoiding per-arm `#ifdef`
  branches that trip clang-tidy
  `readability-function-cognitive-complexity` and JPL-P10 rule 4.
- **NULL-argument validation comes first.** Every public entry
  point checks `if (!arg_a || arg_b < 0) return -EINVAL;` before live
  transport dispatch (or disabled-build `-ENOSYS` stub). Never invert this
  order on rebase — smoke contract depends on it.

## Governing ADRs

- [ADR-0025](../../../../docs/adr/0025-copyright-handling-dual-notice.md) —
  copyright policy; [ADR-0861](../../../../docs/adr/0861-vmafx-copyright-policy-drop-anthropic.md)
  = current line format.
- [ADR-0209](../../../../docs/adr/0209-mcp-embedded-scaffold.md) —
  audit-first MCP scaffold.
