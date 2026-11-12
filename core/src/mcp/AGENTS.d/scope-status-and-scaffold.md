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
# Embedded MCP scope, status, and scaffold rules

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
mongoose rejected on license grounds — see invariant #6
below). Every public entry point still validates arguments
first (`-EINVAL` on NULLs / negative fds / NULL paths); smoke
test pins both input-validation contract and
live round-trip behaviour.

## Ground rules

- **Parent rules** apply (see [../../AGENTS.md](../../../AGENTS.md)).
- **Wholly-new fork files carry `Copyright 2026 Lusoris`**
  per [ADR-0861](../../../../docs/adr/0861-vmafx-copyright-policy-drop-anthropic.md).
- **Audit-first contract** ([ADR-0209](../../../../docs/adr/0209-mcp-embedded-scaffold.md)):
  every public entry point validates arguments **before**
  returning `-ENOSYS`. Validation must survive runtime PR
  — smoke tests for `_init`, `_start_uds`, `_start_stdio` rely
  on early `-EINVAL` even after runtime arrives.

## Rebase-sensitive invariants

- **Smoke test pins the contract.**
  [`../../test/test_mcp_smoke.c`](../../../test/test_mcp_smoke.c) has
  12 sub-tests asserting per-entry-point return values
  (`-EINVAL` on NULL args, `-ENOSYS` on valid args). Any rebase or
  refactor "succeeding" scaffold (e.g. accidentally enabling
  code path) without flipping smoke expectations breaks
  rebase story for runtime PR. **Runtime PR (T5-2b) is
  ONLY PR allowed to update smoke expectations.**
- **`enable_mcp` umbrella flag defaults `false`**. Silent-flip
  risk = same as ADR-0175's Vulkan precedent. Never flip it
  to `true` until all three transport bodies stable and
  reviewed.
- **Per-build-flag availability**. Umbrella `enable_mcp` flag
  flips `HAVE_MCP`; per-transport sub-flags flip matching
  `HAVE_MCP_*` macros (`HAVE_MCP_SSE`, `HAVE_MCP_UDS`,
  `HAVE_MCP_STDIO`). Header surface identical either way;
  only runtime PR distinguishes built-without (returns
  `-ENOSYS` forever) vs built-with (returns `-ENOSYS` only until
  runtime wired). **On rebase**: keep per-transport
  bitmask fold-pattern in `vmaf_mcp_transport_available` —
  preprocessor-fed arithmetic compiles to constant load +
  bittest at every call site, avoiding per-arm `#ifdef`
  branches that trip clang-tidy
  `readability-function-cognitive-complexity` and JPL-P10 rule 4.
- **NULL-argument validation comes first.** Every public entry
  point's body reads `if (!arg_a || arg_b < 0) return -EINVAL;`,
  then any future runtime body, then fall-through
  `return -ENOSYS;`. Never invert this order on rebase — smoke
  contract depends on it.

## Power-of-10 reservations for the runtime PR

Documented for forward-looking discipline (not enforced
by code yet — runtime PR makes them load-bearing):

- **No alloc on measurement-thread hot path** (rule 3). Runtime
  PR uses pre-sized SPSC ring buffer drained at frame
  boundaries; measurement thread never calls `malloc`.
- **Bounded drain loops** (rule 2). Every loop in future
  runtime body has static upper bound on iteration count.

## Governing ADRs

- [ADR-0025](../../../../docs/adr/0025-copyright-handling-dual-notice.md) —
  copyright policy; [ADR-0861](../../../../docs/adr/0861-vmafx-copyright-policy-drop-anthropic.md)
  = current line format.
- [ADR-0209](../../../../docs/adr/0209-mcp-embedded-scaffold.md) —
  audit-first MCP scaffold.
