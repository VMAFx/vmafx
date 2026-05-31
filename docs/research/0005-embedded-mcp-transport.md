<!-- markdownlint-disable MD013 MD060 -->
# Research-0005: Embedded MCP in libvmaf — threading, JSON library, SSE server, Power-of-10 fit

- **Status**: Active
- **Workstream**: [ADR-0128](../adr/0128-embedded-mcp-in-libvmaf.md)
- **Last updated**: 2026-04-20

## Question

If we embed a JSON-RPC MCP server inside `libvmaf.so`, exposing SSE + Unix
domain socket + stdio transports, what are the concrete library and
threading choices? Specifically:

1. How does the MCP thread coexist with the measurement thread without
   violating [Power of 10](../principles.md) rule 3 (no dynamic allocation
   after init on the hot path)?
2. Which JSON library do we vendor?
3. Which embeddable HTTP server do we use for the SSE transport?
4. What MCP spec guarantees do we satisfy vs. relax for the library-
   embedded case vs. the canonical standalone-server case?
5. How do the three transports differ operationally and what does that
   mean for CI?

## Sources

- [Anthropic Model Context Protocol specification 2025-03](https://modelcontextprotocol.io/spec)
  — the normative reference for JSON-RPC framing, tool manifests, and the
  stdio / SSE transport definitions.
- The fork's external MCP server at
  [`mcp-server/vmaf-mcp/`](../../mcp-server/vmaf-mcp/) — Python reference
  for what the embedded server has to match semantically.
- [cJSON](https://github.com/DaveGamble/cJSON) — MIT, single-file
  (one `.c` + one `.h`), ~2k LOC, already used by one VMAF test helper.
- [jansson](https://github.com/akheron/jansson) — MIT, multi-file, widely
  deployed, uses malloc per object.
- [simdjson](https://github.com/simdjson/simdjson) — Apache-2, SIMD
  accelerated, large binary footprint (~1MB).
- [mongoose](https://mongoose.ws/) — MIT / commercial dual-license;
  single `.c` + `.h`, embeddable HTTP + WebSocket + SSE; 6k GitHub stars.
- [libmicrohttpd](https://www.gnu.org/software/libmicrohttpd/) — GPL +
  LGPL dual, mature, large dependency footprint.
- [civetweb](https://github.com/civetweb/civetweb) — MIT, embedded HTTP,
  ~10k LOC.
- [Power of 10 rules](../principles.md) — the governing coding-standards
  document for the measurement-thread hot path.
- [ADR-0122](../adr/0122-cuda-gencode-coverage-and-init-hardening.md) —
  precedent for dlopen-based optional dependencies.

## Findings

### 1. Threading model — why two threads, not one

The measurement thread runs the VMAF frame loop: in the hot case it is
consuming frames from FFmpeg and dispatching per-feature kernels. It
must not block on socket I/O, must not allocate, and must not
synchronise with anything slower than an atomic. Per Power of 10:

- Rule 2: all loops have a bounded upper iteration count.
- Rule 3: no dynamic memory allocation after init.
- Rule 6: data scope declared at the lowest possible level.

JSON parsing and socket I/O violate all three: parsing is
data-dependent in length, allocates per token, and blocks on read.

The separation of concerns:

- **MCP thread** (`pthread_create` in `vmaf_mcp_start`): owns the socket,
  owns JSON parsing, owns all JSON-allocated buffers. Blocks on `poll()`
  between requests. Does not touch the measurement state directly —
  instead posts command envelopes into an SPSC ring.
- **Measurement thread**: at each frame boundary, drains at most `N`
  command envelopes from the ring (bounded loop, `N = 4` for v1), runs
  them, writes responses back into a second SPSC ring owned by the MCP
  thread. Never blocks — if the queue is full, it drops the envelope
  and the MCP thread turns the drop into a `VMAF_ERR_MCP_QUEUE_FULL`
  JSON-RPC error reply to the agent.

SPSC ring sizes are fixed at `vmaf_mcp_start` from a user-supplied
`VmafMcpConfig.queue_depth` (default 64). All ring slots are a
fixed-size C struct of POD values plus one 512-byte scratch for the
tool arguments; no pointers to heap-owned data cross the boundary.
This is the same pattern the Linux kernel's uring uses.

### 2. JSON library — cJSON wins

| Library | LOC | Alloc strategy | License | Single-file? | Verdict |
|---|---|---|---|---|---|
| **cJSON** | ~2k | per-node malloc | MIT | Yes | Chosen |
| jansson | ~6k | per-node malloc | MIT | No | Ecosystem overlap; doesn't help |
| simdjson | ~80k | lazy/zero-copy, large bin | Apache-2 | No | Overkill; C++; adds 1MB |
| jsmn | ~300 | zero-alloc, user buffers | MIT | Yes | Minimalist; weak error reporting |
| hand-rolled | ~500-800 | user-provided | n/a | n/a | Not worth it |

The decisive factors:

- cJSON is already referenced by one test helper in the tree (trivial to
  share the vendor copy).
- JSON parsing happens **on the MCP thread only**, never on the hot
  path, so the `malloc` footprint doesn't interact with Power of 10 rule
  3 — rule 3 scopes to the hot loop, not the entire process.
- Single-file vendor means one directory under `subprojects/cJSON/` and
  no `pkg-config` / autotools surface.
- Error-reporting quality on malformed requests is important for agent
  debuggability, and cJSON's is adequate (jsmn's is minimal).

Memory model: each incoming JSON-RPC request is parsed into a cJSON
tree, the relevant fields copied into the fixed-size command envelope,
and the tree freed immediately. No cJSON pointers live longer than a
single `poll()` iteration. This makes leak auditing by Valgrind / ASan
trivial.

### 3. SSE server — mongoose wins

| Library | Size | SSE? | License | Embedded-friendly | Verdict |
|---|---|---|---|---|---|
| **mongoose** | ~30k LOC single-file | Yes native | MIT (or commercial) | Yes — explicitly the use case | Chosen |
| libmicrohttpd | ~25k LOC, many files | Yes | LGPL | Partial — hooks are awkward | License problematic for static link in proprietary pipelines |
| civetweb | ~10k LOC | Yes (with config) | MIT | Yes | Close second; no ergonomic edge over mongoose |
| raw `select` + hand-rolled HTTP | — | Would have to build | n/a | Yes | We'd spend the time reimplementing SSE chunked-transfer; no |

Mongoose's MIT variant has one caveat the README flags: the MIT license
restricts you to non-revenue-generating use. For libvmaf, which is a
library that other projects link, this is fine because libvmaf itself
generates no revenue — and any downstream can buy the commercial licence
separately if they need to. We document the constraint in
`docs/mcp/embedded.md` rather than hide it.

The SSE transport wires as follows:

- Mongoose listens on `127.0.0.1:<port>` only. `vmaf_mcp_start` refuses
  to bind to a non-loopback address in v1.
- Each MCP client `GET /mcp/sse` gets a dedicated SSE stream. Request
  payloads arrive as `POST /mcp/request` with an `X-MCP-Session` header
  correlating to the SSE stream.
- The mongoose event loop is the MCP thread's `poll`; no extra thread
  is spawned for HTTP.

### 4. UDS transport — why we're adding a non-spec transport

The MCP spec defines stdio and SSE. UDS is a fork-local extension. We
add it because:

- systemd-managed pipelines expose services over Unix sockets as a
  norm.
- Filesystem permissions give us a cleaner auth model than loopback
  TCP, which any user on the same box can hit.
- FFmpeg's hwcontext ecosystem (VAAPI, DRM PRIME) already assumes Unix
  sockets for IPC between ffmpeg and the compositor.

We mark UDS as "libvmaf extension, not MCP-spec". Agents that speak
only spec MCP use SSE. Agents written for the Linux server niche can
pick UDS for lower overhead and filesystem-based auth.

The UDS framing is **newline-delimited JSON-RPC** — one JSON-RPC object
per line, server reply on the same connection. Matches what systemd's
`sd_notify` and most "Unix socket JSON-RPC" daemons use.

### 5. stdio transport — last and simplest

stdio reuses mongoose's parser for JSON framing (LSP-style
`Content-Length:` headers) by feeding `stdin` bytes through the same
state machine the SSE `POST /mcp/request` path uses. The complication
in the library case — stdio already belongs to the host — is solved by
making stdio opt-in: the host's CLI has to pass
`--enable-mcp-stdio` explicitly, and `vmaf_mcp_start` takes an fd pair
argument so non-CLI hosts can hand over specific file descriptors that
aren't the process's own stdin/stdout.

This also means the CLI exposes stdio under a different surface than
its own JSON output: stdio MCP goes on fd 3 / fd 4 (inheritable,
parent-spawned), not fd 0 / fd 1. We document the fd convention in
`docs/mcp/embedded.md`.

### 6. Power of 10 compatibility

Mapping each rule to the embedded-MCP design:

| Rule | Statement | Embedded MCP compliance |
|---|---|---|
| 1 | No goto / setjmp / recursion | cJSON parses recursively — but on the MCP thread, not the measurement thread. Measurement-thread queue drain is iterative. |
| 2 | Bounded loops | MCP-thread `poll()` loop has request-handling cap (`max_requests_per_tick = 16`). Measurement-thread drain caps at `N` per frame. |
| 3 | No malloc after init on hot path | SPSC rings pre-allocated at `vmaf_mcp_start`. Measurement thread never allocates. MCP thread allocates per request but on its own heap arena, bounded by `queue_depth`. |
| 4 | Functions ≤ 60 lines | Normal style; enforce in review. |
| 5 | ≥ 2 assertions per function | We use the existing `VMAF_ASSERT` pattern. Queue enqueue/dequeue each carry pre- and post-conditions. |
| 6 | Declare scope at smallest level | Standard C practice. |
| 7 | Check every non-void return | `pthread_create`, `poll`, `socket`, `bind`, `listen`, `accept`, every cJSON function — all checked. |
| 8 | Preprocessor discipline | No clever macros in the MCP layer. |
| 9 | Pointer use kept simple | Command envelopes are POD, no function pointers to user data. |
| 10 | Compile clean at highest warning level | Maintained by the existing `.clang-tidy` config. |

Measurement-thread hot path stays under the rules by construction: the
only new code it runs is the "drain ≤ N envelopes" loop, which is
bounded, non-blocking, allocation-free, and calls into existing
feature-extractor APIs. The MCP thread runs under normal C-best-practice
discipline rather than flight-software rigidity; this is explicit
because the MCP thread is bounded (1 per running MCP server) and off
the hot path.

### 7. CI strategy

Each transport gets its own smoke-test:

- **SSE**: spawn `vmaf` with `--enable-mcp-sse=<port>`, `curl -N` the SSE
  stream, `curl -X POST` a `vmaf.status` request, assert the SSE stream
  contains a reply frame within 2 s.
- **UDS**: spawn `vmaf` with `--enable-mcp-uds=/tmp/vmaf-mcp.sock`,
  `socat -` to the socket, send a line-delimited JSON-RPC call.
- **stdio**: spawn `vmaf --enable-mcp-stdio`, talk LSP-framed JSON-RPC
  on fd 3 / fd 4, assert reply.

All three run only when `enable_mcp_embedded=true`. The default CI
matrix leg stays at `enable_mcp_embedded=false` — the embedded-MCP CI
leg is additive, one leg.

## Answered questions (for the ADR)

- **Transport surface?** All three: SSE, UDS, stdio — chosen in the
  AskUserQuestion round on 2026-04-20.
- **JSON library?** cJSON, vendored under `subprojects/cJSON/`.
- **SSE server?** Mongoose, MIT variant, vendored under
  `subprojects/mongoose/`.
- **Build flag?** `-Denable_mcp_embedded=false` default, plus per-
  transport sub-flags for distro maintainers.
- **Threading?** Dedicated MCP pthread plus two SPSC rings; measurement
  thread untouched except for bounded drain at frame boundaries.
- **MCP spec conformance?** SSE and stdio follow spec; UDS is marked
  a fork extension.

## Open questions (for follow-up iterations)

- **Session management**: if an agent disconnects mid-measurement, do
  we reset state or preserve it for reconnect? Defer to the v1
  implementation PR after testing with Claude Desktop.
- **Tool-version negotiation**: the MCP spec supports capability
  handshakes. v1 hardcodes `vmaf.*` tool versions; v2 might need a
  handshake once we add mutating tools.
- **Per-transport rate limiting**: an agent could flood the request
  queue. For v1 we rely on the bounded queue turning excess into
  `QUEUE_FULL` errors; a token bucket is a v2 concern.
- **TLS for SSE**: v1 is loopback-only, so TLS is unnecessary. If a
  future v2 allows network-exposed SSE, TLS via mbedTLS is the
  natural next step — mongoose supports it directly.
- **Integration with the existing Python MCP server**: should the
  embedded server delegate unsupported tools to the external Python
  server via a bridge? Probably not — muddles the architecture — but
  worth noting so we don't accidentally ship duplicative tool names.

## Next steps

1. Governance PR (this one) lands — unblocks the implementation ADRs.
2. Skeleton PR: `core/src/mcp/` runtime scaffold, empty transport
   stubs, `libvmaf_mcp.h` header, build flags wired, cJSON + mongoose
   vendored.
3. SSE transport PR: end-to-end `vmaf.status` over SSE against a test
   harness agent.
4. UDS transport PR: same with `socat` in the smoke test.
5. stdio transport PR: same with fd 3 / fd 4 framing.
6. Tool-surface expansion PR: `vmaf.request_model_swap` — the first
   mutating tool. Separate PR so the hot-swap atomic is auditable on
   its own.
7. Documentation PR: `docs/api/libvmaf_mcp.md` + `docs/mcp/embedded.md`
   (the user-facing guide) + updated `docs/mcp/index.md` pointing to
   both the embedded and external servers.
