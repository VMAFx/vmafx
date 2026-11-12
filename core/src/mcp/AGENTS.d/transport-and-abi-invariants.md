---
paths:
  - core/src/mcp/*.c
  - core/src/mcp/*.h
  - core/src/mcp/meson.build
  - core/include/libvmaf/libvmaf_mcp.h
invariant: UDS 0700 mode, ephemeral compute_vmaf context, cJSON delta, POSIX-socket loopback SSE, and EINTR retries.
---
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
   Teardown is dependency ordered and fail-closed: retry `vmaf_close()` once,
   destroy the request model only after close returns exactly zero, and retain
   both context and model through process exit after a persistent failure.
5. **Vendored cJSON = v1.7.19 plus fork delta, NOT verbatim.**
   `3rdparty/cJSON/cJSON.c` carries banned-function replacements
   (ADR-0683 / ADR-1061), `cJSON_GetArraySize` saturation, ADR-1142
   rework, `saturate_to_int` for NaN. Refresh = re-download upstream,
   **re-apply that delta**, update `3rdparty/cJSON/LICENSE` in same
   commit. [`3rdparty/cJSON/AGENTS.md`](../3rdparty/cJSON/AGENTS.md) lists
   delta + gates. Rule used to read "verbatim, do NOT patch it locally",
   contradicting invariant further down same file; PR #883 followed it
   and reverted two security fixes
   (`T-VENDORED-CJSON-BANNED-FUNCTIONS-REVERTED-2026-09-19`).
6. **SSE transport is fork-owned plain POSIX sockets — NOT mongoose.**
   Original v3 plan to vendor cesanta/mongoose was reversed.
   Mongoose 7.18 is GPL-2.0-only OR commercial; ADR-0332
   judged licence blocker for vendoring into this tree
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
9. **`sse_emit_event` owns SSE frame construction.** The v3 ready
   event already uses this helper; a future broadcast implementation
   may reuse it for POST replies. Do not retain speculative private
   helpers behind `unused` attributes: add response-id extraction only
   when the broadcast path has a caller and tests. The v3 SSE worker
   serves one client synchronously, so its socket writes need no mutex;
   introduce per-stream locking only with concurrent broadcast writers.
10. **Every `read(2)` on blocking fd must retry on `EINTR`.**
    Primary helpers (`read_line`, `sse_read_n`, `read_exact`) and
    over-length line drain loops in `transport_stdio.c` and
    `transport_uds.c` all follow pattern: `if (r < 0 && errno
    == EINTR) continue;` within an explicit retry budget, then break
    on exhausted retries, hard error, EOF, or `\n`.
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
