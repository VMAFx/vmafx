<!-- markdownlint-disable MD025 -->
# AGENTS.md — core/src/mcp

Orientation for agents working on embedded MCP server.
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
All three transports = real implementations: stdio,
`AF_UNIX` UDS (mode 0700, one client at a time), and
loopback-only HTTP/1.1 + SSE (fork-owned plain POSIX sockets;
mongoose rejected on license grounds — see invariant #6
below). Every public entry point still validates arguments
first (`-EINVAL` on NULLs / negative fds / NULL paths); smoke
test pins both input-validation contract and
live round-trip behaviour.

## Ground rules

- **Parent rules** apply (see [../../AGENTS.md](../../AGENTS.md)).
- **Wholly-new fork file** — uses dual Lusoris/Claude (Anthropic)
  copyright header per [ADR-0025](../../../docs/adr/0025-copyright-handling-dual-notice.md).
- **Audit-first contract** ([ADR-0209](../../../docs/adr/0209-mcp-embedded-scaffold.md)):
  every public entry point validates arguments **before**
  returning `-ENOSYS`. Validation must survive runtime PR
  — smoke tests for `_init`, `_start_uds`, `_start_stdio` rely
  on early `-EINVAL` even after runtime arrives.

## Rebase-sensitive invariants

- **Smoke test pins the contract.**
  [`../../test/test_mcp_smoke.c`](../../test/test_mcp_smoke.c) has
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

- [ADR-0025](../../../docs/adr/0025-copyright-handling-dual-notice.md) —
  dual-copyright policy.
- [ADR-0209](../../../docs/adr/0209-mcp-embedded-scaffold.md) —
  audit-first MCP scaffold.

# `core/src/mcp/` — agent-relevant invariants

Fork-local subtree. Read this before editing any TU under
`core/src/mcp/`.

## Rebase-sensitive invariants (ADR-0108)

1. **Entire subtree is fork-local.** Netflix/vmaf upstream has
   no embedded MCP surface. If future upstream sync introduces
   colliding `mcp/` directory, expect port-only resolution —
   names collide, semantics may not.
2. **Public ABI lives in `core/include/libvmaf/libvmaf_mcp.h`**;
   `mcp_internal.h` is implementation-only. ABI breaks require
   ADR per CLAUDE §12 r8.
3. **UDS socket file is mode 0700** (owner-only). `chmod`
   happens in `vmaf_mcp_start_uds` after `bind`, is
   load-bearing security invariant per ADR-0128. Never relax it.
4. **`compute_vmaf` uses per-call ephemeral `VmafContext`.** Never
   rewire it to reuse `server->ctx`: `vmaf_score_pooled`
   commits model destructively to context — would
   corrupt host's main measurement run. Tool accepts YUV420p
   8/10/12/16-bit inputs only; adding 4:2:2 / 4:4:4 requires
   `pixel_format` schema extension, docs, and tests in same PR.
5. **Vendored cJSON = v1.7.19 plus fork delta, NOT verbatim.**
   `3rdparty/cJSON/cJSON.c` carries banned-function replacements
   (ADR-0683 / ADR-1061), `cJSON_GetArraySize` saturation, ADR-1142
   rework, `saturate_to_int` for NaN. Refresh = re-download upstream,
   **re-apply that delta**, update `3rdparty/cJSON/LICENSE` in same
   commit. [`3rdparty/cJSON/AGENTS.md`](3rdparty/cJSON/AGENTS.md) lists
   delta + gates. Rule used to read "verbatim, do NOT patch it locally",
   contradicting invariant further down same file; PR #883 followed it
   and reverted two security fixes
   (`T-VENDORED-CJSON-BANNED-FUNCTIONS-REVERTED-2026-09-19`).
6. **SSE transport is fork-owned plain POSIX sockets — NOT mongoose.**
   Original v3 plan to vendor cesanta/mongoose was reversed
   because mongoose 7.18 is GPL-2.0-only OR commercial,
   incompatible with fork's BSD-3-Clause-Plus-Patent license
   (verified 2026-05-09). Never re-introduce mongoose (or any
   GPL-licensed HTTP library) without first amending CLAUDE §1,
   adding separate license-compatibility ADR. Minimal
   HTTP/1.1 + SSE surface lives in `transport_sse.c` (~500 LOC).
7. **SSE listener-shutdown uses `shutdown(SHUT_RDWR)` before
   `close()`.** Plain `close()` of AF_INET listening fd from
   another thread does NOT unblock `accept()` on Linux. UDS
   transport (AF_UNIX) does not need this; SSE transport does.
   Smoke test `test_sse_event_stream` regresses if
   shutdown call removed (test hangs waiting for join).
8. **SSE binds `INADDR_LOOPBACK` only.** Never switch to
   `INADDR_ANY` without ADR + auth design — v3 explicitly ships
   without CORS/Bearer/per-session auth on assumption of
   same-host trust boundary.
9. **`sse_emit_event` and `sse_extract_id` are reserved for v4
   broadcast.** Marked `__attribute__((unused))` in v3 to keep
   build warning-free; v4 routes POST replies onto subscribed
   GET streams via these helpers.
10. **Every `read(2)` on blocking fd must retry on `EINTR`.**
    Primary helpers (`read_line`, `sse_read_n`, `read_exact`) and
    over-length line drain loops in `transport_stdio.c` and
    `transport_uds.c` all follow pattern: `if (r < 0 && errno
    == EINTR) continue;` then break on hard error / EOF / `\n`.
    Never collapse this to `if (r <= 0) break;` — stray signal
    (SIGCHLD, SIGURG, debugger attach) then desynchronises
    stream framing on very next request. Same for `write(2)` —
    every fork-added write site loops `off < len`, retries on
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

`PDJSON_STACK_MAX` is defined to `512` at top of
`core/src/pdjson.c` (before `#ifdef` guard). This definition
must survive any future vendor sync. If replacing `pdjson.c` with
newer upstream release, check `PDJSON_STACK_MAX` either
defined by build system or re-added at top of file.
512 levels is well beyond any VMAF model or MCP message depth.

## Invariant: cJSON banned-function-free (ADR-0683 / ADR-1061)

`3rdparty/cJSON/cJSON.c` stays free of `sprintf`, `strcpy`, `strcat`,
`strtok`, `atoi`, `atof`, `gets`, `rand`, `system`. Enforced, not
remembered: `vmaf-no-strcpy-strcat-sprintf` Semgrep rule covers this
directory (hook `semgrep-local`, CI job `Semgrep`), and
`scripts/ci/tests/test_semgrep_vendored_scope.py` (hook
`test-semgrep-vendored-scope`) fails if path exclude or `.semgrepignore`
line ever hides it again -> that is what let the 1.7.19 re-vendor bring
eleven banned calls back unnoticed. Verify:

```bash
python3 -m unittest discover -s scripts/ci/tests -p test_semgrep_vendored_scope.py
```

Future cJSON version sync re-applies fork delta listed in
[`3rdparty/cJSON/AGENTS.md`](3rdparty/cJSON/AGENTS.md). Never answer a
finding here with an exclusion.

## Smoke test

```text
build/test/test_mcp_smoke   # expects "17 tests run, 17 passed"
```

v3 sub-test `test_sse_event_stream` spawns SSE server on
ephemeral loopback port, performs `GET /mcp/sse`, checks
for `Content-Type: text/event-stream`, `event: ready` field,
`data:` field, and blank-line frame terminator (per WHATWG
SSE §9.2, accessed 2026-05-09); then performs `POST /mcp/sse`
with `tools/list` JSON-RPC request, verifies inline
response contains `list_features`. v2 sub-tests
`test_uds_roundtrip` and `test_compute_vmaf_real_score` remain.

## Path allowlist parity (R2-4)

`compute_vmaf.c` `validate_path()` / `build_allowed_roots()` MUST keep same
allowlisted root set as Python (`_allowed_roots`) and Go (`AllowedRoots`)
MCP servers; change all three together. Guarded by
`test_mcp_compute_vmaf_allowlist.c`.
