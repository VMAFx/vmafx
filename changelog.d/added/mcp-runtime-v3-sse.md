### lusoris fork

- **mcp**: runtime v3 lands the SSE (Server-Sent Events over
  loopback HTTP) transport that v2 deferred. `vmaf_mcp_start_sse`
  is no longer `-ENOSYS`; it binds an HTTP/1.1 listener on
  `127.0.0.1` (configurable port; default 0 → kernel-picked
  ephemeral) serving `GET /mcp/sse` (event-stream) and
  `POST /mcp/sse` (inline JSON-RPC) per WHATWG SSE §9.2. The
  transport is implemented in plain POSIX sockets (~500 LOC
  fork-owned C); the originally-planned mongoose vendor was
  reversed during pre-vendor license verification because
  cesanta/mongoose 7.18 is GPL-2.0-only OR commercial,
  incompatible with the fork's BSD-3-Clause-Plus-Patent license.
  Build flag `enable_mcp_sse` was promoted from `boolean` to
  `feature` (`auto` default) to match the project-wide
  transport-flag convention. See
  [ADR-0332](../../docs/adr/0332-mcp-runtime-v2.md) §
  "Status update 2026-05-09 (v3 SSE)" and
  [`docs/mcp/embedded.md`](../../docs/mcp/embedded.md) for the
  curl smoke pattern + browser EventSource usage.
