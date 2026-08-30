# Research-0090: MCP runtime v3 SSE transport implementation choice

- **Date**: 2026-05-09
- **Status**: Resolved (informs ADR-0332 § "Status update 2026-05-09 (v3 SSE)")
- **Tags**: mcp, transport, sse, http, license, vendoring

## Question

The v3 task brief pre-decided the implementation: vendor
cesanta/mongoose (`core/src/mcp/3rdparty/mongoose/`) and wire
its SSE feature set into a new `transport_sse.c`. Pre-vendor
verification surfaced a license-compatibility blocker. Two
sub-questions opened:

1. Is mongoose's stated license actually compatible with the
   fork's BSD-3-Clause-Plus-Patent terms?
2. If not, what is the cheapest path to ship the SSE transport
   in this PR rather than deferring it again?

## Findings

### Mongoose 7.18 license — verified at upstream

Per the upstream LICENSE at the 7.18 tag (accessed 2026-05-09:
<https://github.com/cesanta/mongoose/blob/7.18/LICENSE>):

> Copyright (c) 2004-2013 Sergey Lyubka
> Copyright (c) 2013-2025 Cesanta Software Limited
> All rights reserved
>
> This software is dual-licensed: you can redistribute it and/or
> modify it under the terms of the GNU General Public License
> version 2 as published by the Free Software Foundation. […]
>
> Alternatively, you can license this software under a commercial
> license, as set out in <https://mongoose.ws/licensing/>.

`mongoose.h` carries `SPDX-License-Identifier: GPL-2.0-only or
commercial`. The combined work (libvmaf + mongoose) under the
GPL-2 leg would force GPL distribution terms onto every
downstream that links libvmaf, which contradicts CLAUDE.md §1
("License: BSD-3-Clause-Plus-Patent ... preserved"). The
commercial leg requires a per-deployment paid license and is
not viable for a permissively-licensed open-source fork.

The task brief's claim "MIT license (BSD-compatible)" is
factually wrong — mongoose has never shipped under MIT. The
brief's reference link <https://github.com/cesanta/mongoose>
itself surfaces the GPL-2 terms in the LICENSE; the
verification step is what caught the error.

### Alternatives considered

| Option | LOC vendored | License risk | Audit load | Verdict |
| --- | --- | --- | --- | --- |
| cesanta/mongoose 7.18 | ~28 000 | GPL-2 blocker | High | Rejected (license) |
| libwebsockets | ~120 000 | LGPL with linker exception | Medium | Heavyweight; rejected |
| nopoll | ~30 000 | LGPL-2.1 | Medium | LGPL is a softer issue but still requires care; rejected |
| h2o (h2o/h2o) | ~80 000 | MIT | High | Pulls a TLS stack and an event loop the fork does not need |
| picohttpparser | ~500 | MIT | Low | Permissive, but parser-only — does not implement an HTTP server |
| Roll our own minimal HTTP+SSE in plain POSIX sockets | ~500 | None | Low | Chosen |

The minimal HTTP/1.1 surface SSE actually requires is a
request-line parser, a header drainer that captures
Content-Length, a writer with a per-connection mutex, and a
Server-Sent-Events frame emitter. The fork's existing UDS
transport already implements four of those primitives;
extending them to AF_INET costs a few dozen lines.

The chosen path adds ~500 LOC of fork-owned C to
`core/src/mcp/transport_sse.c`, comparable in size to the
existing `transport_uds.c` (179 LOC) plus the HTTP-specific
helpers. The SSE framing logic itself is ~30 LOC because
WHATWG SSE §9.2 is a small spec.

### Linux listener-shutdown gotcha

Empirical verification: closing an AF_INET listening fd from a
second thread does NOT unblock `accept()` on Linux 6.x kernels.
A reproducer (in this session's scratch) ran `pthread_join` on
a thread blocked in `accept()` after `close(listen_fd)` from
the main thread; the join hung indefinitely. Replacing
`close()` with `shutdown(listen_fd, SHUT_RDWR)` followed by
`close()` released the worker. The behaviour is consistent with
several Stack Overflow reports and the kernel's TCP stack
implementation but is not explicitly called out in `accept(2)`.
The UDS transport (AF_UNIX) does not need `shutdown` —
plain `close()` does unblock the accept loop, which matches
the working PR #533 v2 test.

This finding is encoded in `vmaf_mcp_stop`'s SSE branch and
documented in `docs/mcp/embedded.md` § "Listener-shutdown
invariant" + `core/src/mcp/AGENTS.md`.

## Recommendation

Implement the SSE transport in plain POSIX sockets in
`core/src/mcp/transport_sse.c` with the `shutdown`-first
stop sequence; do not vendor mongoose; do not vendor any
GPL-licensed alternative. Update CLAUDE.md §1 first if a
future PR ever needs to revisit this trade-off.

## References

- WHATWG HTML Living Standard §9.2 "Server-sent events"
  (accessed 2026-05-09):
  <https://html.spec.whatwg.org/multipage/server-sent-events.html>
- IETF RFC 9110 "HTTP Semantics" (accessed 2026-05-09):
  <https://www.rfc-editor.org/rfc/rfc9110.html>
- cesanta/mongoose LICENSE at 7.18 (accessed 2026-05-09):
  <https://github.com/cesanta/mongoose/blob/7.18/LICENSE>
- GNU GPL v2 OR commercial dual-license note in
  `mongoose.h@7.18`: SPDX-License-Identifier line.
- `accept(2)` on Linux: <https://man7.org/linux/man-pages/man2/accept.2.html>
- Source: req — task brief for "MCP runtime v3 — ship the SSE
  transport that v2 deferred" (paraphrased).
