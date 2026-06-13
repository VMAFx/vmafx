<!-- markdownlint-disable MD025 -->
# AGENTS.md — core/src/mcp

Orientation for agents working on the embedded MCP server.
Parent: [../../AGENTS.md](../../AGENTS.md).

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

Public C-API: [`../../include/libvmaf/libvmaf_mcp.h`](../../include/libvmaf/libvmaf_mcp.h).
Smoke test: [`../../test/test_mcp_smoke.c`](../../test/test_mcp_smoke.c).

## Backend status

**Live** (T5-2b + v2 + v3, [ADR-0209](../../../docs/adr/0209-mcp-embedded-scaffold.md)).
All three transports are real implementations: stdio,
`AF_UNIX` UDS (mode 0700, single client at a time), and
loopback-only HTTP/1.1 + SSE (fork-owned plain POSIX sockets;
mongoose was rejected on license grounds — see invariant #6
below). Every public entry point still validates its arguments
first (`-EINVAL` on NULLs / negative fds / NULL paths); the
smoke test pins both the input-validation contract and the
live round-trip behaviour.

## Ground rules

- **Parent rules** apply (see [../../AGENTS.md](../../AGENTS.md)).
- **Wholly-new fork file** — uses the dual Lusoris/Claude (Anthropic)
  copyright header per [ADR-0025](../../../docs/adr/0025-copyright-handling-dual-notice.md).
- **Audit-first contract** ([ADR-0209](../../../docs/adr/0209-mcp-embedded-scaffold.md)):
  every public entry point validates its arguments **before**
  returning `-ENOSYS`. The validation must survive the runtime PR
  — the smoke tests for `_init`, `_start_uds`, `_start_stdio` rely
  on early `-EINVAL` even after the runtime arrives.

## Rebase-sensitive invariants

- **The smoke test pins the contract.**
  [`../../test/test_mcp_smoke.c`](../../test/test_mcp_smoke.c) has
  12 sub-tests asserting per-entry-point return values
  (`-EINVAL` on NULL args, `-ENOSYS` on valid args). Any rebase or
  refactor that "succeeds" the scaffold (e.g. accidentally enables
  a code path) without flipping the smoke expectations breaks the
  rebase story for the runtime PR. **The runtime PR (T5-2b) is the
  ONLY PR allowed to update the smoke expectations.**
- **`enable_mcp` umbrella flag defaults `false`**. The silent-flip
  risk is the same as ADR-0175's Vulkan precedent. Do not flip it
  to `true` until all three transport bodies are stable and
  reviewed.
- **Per-build-flag availability**. The umbrella `enable_mcp` flag
  flips `HAVE_MCP`; per-transport sub-flags flip the matching
  `HAVE_MCP_*` macros (`HAVE_MCP_SSE`, `HAVE_MCP_UDS`,
  `HAVE_MCP_STDIO`). The header surface is identical either way;
  only the runtime PR distinguishes built-without (returns
  `-ENOSYS` forever) vs built-with (returns `-ENOSYS` only until
  the runtime is wired). **On rebase**: keep the per-transport
  bitmask fold-pattern in `vmaf_mcp_transport_available` — the
  preprocessor-fed arithmetic compiles to a constant load +
  bittest at every call site, which avoids per-arm `#ifdef`
  branches that trip clang-tidy
  `readability-function-cognitive-complexity` and JPL-P10 rule 4.
- **NULL-argument validation comes first.** Every public entry
  point's body reads `if (!arg_a || arg_b < 0) return -EINVAL;`,
  then any future runtime body, then a fall-through
  `return -ENOSYS;`. Do not invert this order on rebase — the
  smoke contract depends on it.

## Power-of-10 reservations for the runtime PR

Documented for forward-looking discipline (these are not enforced
by code yet — the runtime PR makes them load-bearing):

- **No alloc on the measurement-thread hot path** (rule 3). The
  runtime PR uses a pre-sized SPSC ring buffer drained at frame
  boundaries; the measurement thread never calls `malloc`.
- **Bounded drain loops** (rule 2). Every loop in the future
  runtime body has a static upper bound on iteration count.

## Governing ADRs

- [ADR-0025](../../../docs/adr/0025-copyright-handling-dual-notice.md) —
  dual-copyright policy.
- [ADR-0209](../../../docs/adr/0209-mcp-embedded-scaffold.md) —
  audit-first MCP scaffold.

# `core/src/mcp/` — agent-relevant invariants

Fork-local subtree. Read this before editing any TU under
`core/src/mcp/`.

## Rebase-sensitive invariants (ADR-0108)

1. **The entire subtree is fork-local.** Netflix/vmaf upstream has
   no embedded MCP surface. If a future upstream sync introduces
   a colliding `mcp/` directory, expect a port-only resolution —
   names collide, semantics may not.
2. **Public ABI lives in `core/include/libvmaf/libvmaf_mcp.h`**;
   `mcp_internal.h` is implementation-only. ABI breaks require an
   ADR per CLAUDE §12 r8.
3. **UDS socket file is mode 0700** (owner-only). The `chmod`
   happens in `vmaf_mcp_start_uds` after `bind` and is a
   load-bearing security invariant per ADR-0128. Do NOT relax it.
4. **`compute_vmaf` uses a per-call ephemeral `VmafContext`.** Do
   NOT rewire it to reuse `server->ctx`: `vmaf_score_pooled`
   commits the model destructively to the context, which would
   corrupt the host's main measurement run. The tool accepts YUV420p
   8/10/12/16-bit inputs only; adding 4:2:2 / 4:4:4 requires a
   `pixel_format` schema extension, docs, and tests in the same PR.
5. **Vendored cJSON v1.7.18 is verbatim** under MIT. Do NOT patch
   it locally — refresh by re-downloading from upstream
   `DaveGamble/cJSON` and update `3rdparty/cJSON/LICENSE` in the
   same commit.
6. **SSE transport is fork-owned plain POSIX sockets — NOT mongoose.**
   The original v3 plan to vendor cesanta/mongoose was reversed
   because mongoose 7.18 is GPL-2.0-only OR commercial,
   incompatible with the fork's BSD-3-Clause-Plus-Patent license
   (verified 2026-05-09). Do NOT re-introduce mongoose (or any
   GPL-licensed HTTP library) without first amending CLAUDE §1 and
   adding a separate license-compatibility ADR. The minimal
   HTTP/1.1 + SSE surface lives in `transport_sse.c` (~500 LOC).
7. **SSE listener-shutdown uses `shutdown(SHUT_RDWR)` before
   `close()`.** Plain `close()` of an AF_INET listening fd from
   another thread does NOT unblock `accept()` on Linux. The UDS
   transport (AF_UNIX) does not need this; the SSE transport does.
   The smoke test `test_sse_event_stream` regresses if the
   shutdown call is removed (test hangs waiting for join).
8. **SSE binds `INADDR_LOOPBACK` only.** Do NOT switch to
   `INADDR_ANY` without an ADR + auth design — v3 explicitly ships
   without CORS/Bearer/per-session auth on the assumption of a
   same-host trust boundary.
9. **`sse_emit_event` and `sse_extract_id` are reserved for v4
   broadcast.** Marked `__attribute__((unused))` in v3 to keep the
   build warning-free; v4 will route POST replies onto subscribed
   GET streams via these helpers.
10. **Every `read(2)` on a blocking fd must retry on `EINTR`.** The
    primary helpers (`read_line`, `sse_read_n`, `read_exact`) and
    the over-length line drain loops in `transport_stdio.c` and
    `transport_uds.c` all follow the pattern: `if (r < 0 && errno
    == EINTR) continue;` then break on hard error / EOF / `\n`.
    Do NOT collapse this to `if (r <= 0) break;` — a stray signal
    (SIGCHLD, SIGURG, debugger attach) will then desynchronise the
    stream framing on the very next request. Same for `write(2)` —
    every fork-added write site loops `off < len` and retries on
    `EINTR`. ADR-0872.

## Build flags

```bash
meson setup build -Denable_mcp=true \
                  -Denable_mcp_stdio=true \
                  -Denable_mcp_uds=true \
                  -Denable_mcp_sse=enabled
# enable_mcp_sse is a `feature` option (default: auto). The SSE
# transport is plain POSIX sockets — no third-party vendor probe.
```

## Invariant: pdjson depth limit (ADR-1061)

`PDJSON_STACK_MAX` is defined to `512` at the top of
`core/src/pdjson.c` (before the `#ifdef` guard). This definition
must survive any future vendor sync. If you replace `pdjson.c` with
a newer upstream release, check that `PDJSON_STACK_MAX` is either
defined by the build system or re-added at the top of the file.
512 levels is well beyond any VMAF model or MCP message depth.

## Invariant: cJSON banned-function-free (ADR-0683 / ADR-1061)

`3rdparty/cJSON/cJSON.c` must remain free of `sprintf`, `strcpy`,
`strcat`, `strtok`, `atoi`, `atof`, `gets`, `rand`, `system`.
Verify with:

```bash
grep -n '\bsprintf\b\|\bstrcpy\b\|\bstrcat\b' core/src/mcp/3rdparty/cJSON/cJSON.c
```

The expected output is empty (only comments mentioning these names
are allowed). A future cJSON version sync must re-validate and
re-apply the replacements documented in ADR-0683 / ADR-1061 if the
upstream has not addressed them.

## Smoke test

```text
build/test/test_mcp_smoke   # expects "17 tests run, 17 passed"
```

The v3 sub-test `test_sse_event_stream` spawns the SSE server on
an ephemeral loopback port, performs a `GET /mcp/sse` and checks
for `Content-Type: text/event-stream`, an `event: ready` field, a
`data:` field, and the blank-line frame terminator (per WHATWG
SSE §9.2, accessed 2026-05-09); then performs a `POST /mcp/sse`
with a `tools/list` JSON-RPC request and verifies the inline
response contains `list_features`. The v2 sub-tests
`test_uds_roundtrip` and `test_compute_vmaf_real_score` remain.
