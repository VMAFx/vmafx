# ADR-0402: MCP runtime v2 — UDS transport + real `compute_vmaf` binding

- **Status**: Accepted
- **Date**: 2026-05-09
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: mcp, agents, api, transport, fork-local

## Context

[ADR-0209](0209-mcp-embedded-scaffold.md) shipped the embedded
MCP server as audit-first scaffolding (`-ENOSYS` on every entry
point). PR #490 (T5-2b) flipped `vmaf_mcp_init`,
`vmaf_mcp_start_stdio`, `vmaf_mcp_stop`, `vmaf_mcp_close` plus a
JSON-RPC 2.0 dispatcher with two tools: `list_features` (real)
and `compute_vmaf` (placeholder returning
`{"status":"deferred_to_v2"}`). Two pieces remained deferred:

- **UDS transport** — needed for embedded scenarios where a host
  process drives libvmaf out-of-band over a local filesystem
  socket (mode 0700, owner-only).
- **`compute_vmaf` real implementation** — the placeholder was
  visibly half-finished; AI hosts that called it through the MCP
  surface got no useful output.

A third piece (SSE / loopback HTTP) was originally scoped here as
well but deferred to v3 — see *Alternatives considered*.

## Decision

We will land MCP runtime v2 with two additions on top of PR #490:

1. **UDS transport** in `core/src/mcp/transport_uds.c`. Standard
   POSIX `socket(AF_UNIX, SOCK_STREAM, 0)` + `bind` + `listen` +
   `accept` loop; per-connection serial dispatch through the
   existing `dispatcher.c`; line-delimited JSON-RPC framing
   identical to the stdio transport. The socket file is created
   with `chmod 0700` after `bind` per ADR-0128 § "Operational
   guardrails".
2. **`compute_vmaf` real binding** in `core/src/mcp/compute_vmaf.c`.
   Per-call ephemeral `VmafContext` (so the host's main scoring
   run is not perturbed), `vmaf_model_load` of the requested
   `model_version` (default `vmaf_v0.6.1`), POSIX YUV reader for
   YUV420p 8-bit pairs, `vmaf_read_pictures` per frame, then
   `vmaf_score_pooled` with `VMAF_POOL_METHOD_MEAN`. Returns
   `{score, frames_scored, model_version, pool_method}`.

The smoke test (`core/test/test_mcp_smoke.c`) is extended with
a UDS round-trip and a `compute_vmaf` real-score check against the
testdata 576x324 48-frame YUV pair. The pinned-`-ENOSYS`
expectation for UDS is dropped; SSE remains pinned at `-ENOSYS`
until v3.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Land all three (UDS + SSE + compute_vmaf) in one PR | Single review pass | Mongoose vendoring (50 KB+) needs its own due-diligence pass; would balloon the diff and risk no-guessing rule on SSE-spec wording | Deferred SSE to v3 — focused PR ships clean |
| Reuse the borrowed `server->ctx` for `compute_vmaf` | No per-call allocation | `vmaf_score_pooled` commits the model to the context; out-of-band MCP scoring would corrupt the host's main run | Per-call ephemeral `VmafContext` preserves the contract |
| Drop the file-size sanity check (size%bytes_per_frame == 0) | Simpler reader | Silently truncates frames or scores garbage when the host passes wrong width/height | Reject up front with `-EINVAL` and a structured error message |
| Per-client UDS thread (concurrent clients) | Higher throughput under load | Embedded use case is single-host-driver; thread-pool adds bug surface | Single-client serial accept; v3 may revisit |

## Consequences

- **Positive**: AI hosts driving libvmaf via MCP get a working
  `compute_vmaf` tool; embedded hosts get a UDS surface that
  doesn't claim the host's stdin/stdout.
- **Negative**: `compute_vmaf` allocates a per-call `VmafContext`
  (small, but non-zero); hosts that need batched scoring should
  use the libvmaf CLI, not the MCP tool.
- **Neutral / follow-ups**: SSE / mongoose vendoring still owed
  (v3); 10/12-bit YUV support still owed (v3 widens
  `compute_vmaf`); per-client UDS threading is on the v3 wish-list.

## References

- [ADR-0128](0128-embedded-mcp-in-libvmaf.md) — original embedded-MCP design.
- [ADR-0209](0209-mcp-embedded-scaffold.md) — audit-first scaffold.
- PR #490 — T5-2b stdio runtime + `compute_vmaf` placeholder.
- [Research-0005](../research/0005-embedded-mcp-transport.md) — transport sequencing.
- AF_UNIX(7) Linux man page (accessed 2026-05-09):
  <https://man7.org/linux/man-pages/man7/unix.7.html>
- Source: req — task brief for "MCP runtime v2 — three additions
  to PR #490's stdio-only v1" (paraphrased: ship UDS + SSE +
  real `compute_vmaf`; trim aggressively if needed in priority
  order compute_vmaf > UDS > SSE).

## Status update 2026-05-09: MCP runtime v3 SSE landed (T5-2d)

The SSE transport deferred above has now landed. Delta:

- **No mongoose vendor.** The original v3 plan was to vendor
  cesanta/mongoose (~28k LOC) as the HTTP/SSE library. Pre-vendor
  due-diligence reverified the upstream license at the 7.18 tag
  (<https://github.com/cesanta/mongoose>, accessed 2026-05-09): the
  effective terms are GPL-2.0-only OR a paid commercial license.
  Linking GPL-2-only code into libvmaf would force the combined
  work to GPL terms — incompatible with the fork's
  BSD-3-Clause-Plus-Patent license preserved per CLAUDE.md §1.
  We therefore implement the minimal HTTP/1.1 + SSE surface
  needed in plain POSIX sockets in
  [`core/src/mcp/transport_sse.c`](../../core/src/mcp/transport_sse.c)
  (~500 LOC), reusing the same accept/read/write patterns as
  `transport_uds.c`.
- **Loopback-only HTTP server** on a configurable TCP port
  (default 0 → kernel-picked ephemeral). The bind explicitly uses
  `INADDR_LOOPBACK`; non-loopback exposure would require a
  separate ADR.
- **Two endpoints on the same socket**: `GET /mcp/sse` returns
  `Content-Type: text/event-stream` with a parser-friendly
  `event: ready\ndata: <json>\n\n` initial frame; `POST /mcp/sse`
  accepts a JSON-RPC request body and replies inline with the
  dispatcher's response. SSE-stream broadcast (POST routes the
  reply onto a subscribed GET stream) is reserved for v4 — the
  helper functions `sse_emit_event` and `sse_extract_id` ship
  in v3 marked `__attribute__((unused))` for that path.
- **Listener-shutdown fix.** Plain `close(listen_fd)` from a
  second thread does NOT unblock `accept()` on Linux AF_INET
  (verified empirically); the SSE stop path uses
  `shutdown(SHUT_RDWR)` before `close()` to release the worker.
- **Build wiring.** `enable_mcp_sse` was promoted from `boolean`
  to `feature` (`auto` default) in `core/meson_options.txt`,
  matching the Vulkan/CUDA convention. The flag still gates the
  `HAVE_MCP_SSE` define and `transport_sse.c` is unconditionally
  compiled in when enabled — there are no third-party prereqs
  after the mongoose pivot.
- **Smoke coverage.** `core/test/test_mcp_smoke.c::test_sse_event_stream`
  spawns the server on an ephemeral port, verifies
  `Content-Type: text/event-stream` + the `event:` / `data:` /
  blank-line framing per WHATWG SSE §9.2 (accessed 2026-05-09:
  <https://html.spec.whatwg.org/multipage/server-sent-events.html>),
  and round-trips a `tools/list` POST. The pinned-`-ENOSYS`
  expectation for SSE is dropped; a NULL-cfg negative case
  replaces it.

### Alternatives considered (v3 SSE)

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Vendor cesanta/mongoose 7.18 (original plan) | Battle-tested HTTP/WebSocket/SSE; single-header drop-in; small (~6k LOC the SSE path actually uses) | License is GPL-2.0-only OR commercial — incompatible with BSD-3-Clause-Plus-Patent fork license | License blocker; cannot ship in a BSD library |
| Vendor a different MIT/BSD HTTP library (e.g. mongoose alternatives) | Permissive license | Audit + due-diligence load; new dependency surface; no library is as compact as cesanta/mongoose for the HTTP+SSE combination | The minimal HTTP surface SSE needs is ~500 LOC of fork-owned C; cheaper than a third-party vendor |
| Roll our own minimal HTTP+SSE in plain POSIX sockets (chosen) | No third-party license risk; same accept/read/write patterns as `transport_uds.c`; small attack surface | Hand-rolled HTTP parser; not feature-complete vs. mongoose | Right size for the embedded MCP use case; matches fork's "vendor only when truly necessary" policy |
| Defer SSE to v4 and ship UDS-only | Simplest review | The umbrella MCP server stays missing its remote-friendly transport indefinitely | Embedded HTTP+SSE is small enough to fit alongside v2 work |

### Consequences (v3 SSE)

- **Positive**: AI hosts can subscribe to libvmaf via plain HTTP
  SSE — no UDS / stdio fd plumbing required. The transport is
  testable with `curl -N http://127.0.0.1:<port>/mcp/sse` for
  quick interactive debugging.
- **Negative**: Hand-rolled HTTP parser is narrowly-featured; we
  do not negotiate Connection: keep-alive, do not support gzip,
  do not validate the URL beyond exact-match against the
  configured path. Hosts that need full HTTP semantics must
  proxy through a real HTTP server.
- **Neutral / follow-ups**: v4 will broadcast POST replies on
  the GET stream channel; v4 may also add an optional
  authentication shim (Bearer token) for non-loopback bindings,
  which v3 explicitly forbids.

## References (v3 SSE)

- WHATWG HTML Living Standard §9.2 "Server-sent events"
  (accessed 2026-05-09):
  <https://html.spec.whatwg.org/multipage/server-sent-events.html>
- cesanta/mongoose 7.18 LICENSE (accessed 2026-05-09):
  <https://github.com/cesanta/mongoose/blob/7.18/LICENSE> — confirms
  GPL-2.0-only OR commercial.
- IETF RFC 9110 §6 "Message" (accessed 2026-05-09):
  <https://www.rfc-editor.org/rfc/rfc9110.html> — HTTP semantics
  consulted for the request-line + header parser.
- Source: req — task brief for "MCP runtime v3 — ship the SSE
  transport that v2 deferred" (paraphrased: vendor the chosen
  HTTP library, wire transport_sse.c, smoke-test with a real
  HTTP client). The mongoose vendor was reversed during
  pre-vendor license verification.
