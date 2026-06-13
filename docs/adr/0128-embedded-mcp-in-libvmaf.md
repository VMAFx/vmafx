<!-- markdownlint-disable MD036 MD060 -->
# ADR-0128: Embedded MCP server in libvmaf — SSE + UDS + stdio transports, build-flag-gated

- **Status**: Accepted
- **Status update 2026-05-15**: implemented;
  `core/include/libvmaf/libvmaf_mcp.h` and `core/src/mcp/`
  present on master; `vmaf_mcp_start()` symbol in tree.
- **Date**: 2026-04-20
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: mcp, agents, api, build, docs

## Context

The fork already ships an external Model Context Protocol (MCP)
server at [`mcp-server/vmaf-mcp/`](../../mcp-server/vmaf-mcp/). It
is a Python process that wraps the VMAF CLI, exposes tools to AI
agents (Claude Desktop, Cursor, Zed) over stdio JSON-RPC, and
translates structured requests into CLI invocations. The external
server is the right design when the user workflow is "run a VMAF
comparison and hand the result back to the agent".

A different workflow is emerging: agents that want to
**steer a running measurement**. Examples:

- An agent notices mid-encode that a scene shifted to archival SD
  footage and asks libvmaf to hot-swap from the 4K VMAF model to an
  SD-specific tiny-AI model without dropping the FFmpeg pipeline.
- An agent running a BD-Rate sweep wants to query the current
  frame's per-feature scores while a long-form analysis is in
  flight.
- An automated CI harness wants to pause / resume a measurement
  based on external signals (budget, queue pressure) without
  killing and re-spawning the CLI.

None of these fit the external-server shape — the external server
talks to the CLI, and the CLI has already exited by the time the
agent wants to influence anything.

The user-facing ask from issue #66 is explicit: a "Tiny MCP inside
libvmaf" that exposes agentic-tool JSON-RPC from the C library
itself. The engineering question is how to do that without
violating the [NASA Power of 10 invariants](../principles.md) the
fork enforces, and without bloating the ABI of a library whose
standard consumers are ffmpeg, GStreamer, and the `vmaf` CLI —
none of whom need MCP.

The stock MCP transports are:

- **stdio** — JSON-RPC over the process's own stdin/stdout;
  canonical; how Claude Desktop spawns local MCP servers.
- **SSE over loopback** — JSON-RPC over a Server-Sent-Events HTTP
  stream on localhost; the "remote" transport MCP added in 2024-11.
- **Unix domain socket** — not in the MCP spec; a fork-local
  extension matching how systemd, Docker, and GPU hwcontext
  ecosystems already work.

Stdio is awkward for a library: libvmaf is loaded *into* someone
else's process (ffmpeg, the CLI, a Python harness). Stdio is
already owned by the host; libvmaf can't claim it without colliding
with the host's own I/O. Loopback-socket transports (SSE or UDS)
fit the library shape naturally — they open a socket the agent can
connect to without any stdio contention.

## Decision

We will ship an **embedded MCP server inside libvmaf**, gated
behind a build flag, with **three transport backends selectable at
runtime** and independently buildable. The design:

- **Build flag**: `-Denable_mcp_embedded=false` default. Standard
  release builds do NOT link in the MCP surface. Users opt in
  explicitly; bug risk for the 99% of users who don't run an
  agentic pipeline is zero.
- **Per-transport sub-flags**: `-Denable_mcp_embedded_sse`,
  `-Denable_mcp_embedded_uds`, `-Denable_mcp_embedded_stdio` — each
  default `auto` (on when the parent flag is on, off otherwise).
  Lets distro maintainers ship a subset cleanly.
- **Public API**: one new header
  `core/include/libvmaf/libvmaf_mcp.h`. Functions
  `vmaf_mcp_start(VmafContext *, VmafMcpConfig *, VmafMcpServer **)`
  and `vmaf_mcp_stop(VmafMcpServer *)`. Idempotent; thread-safe;
  can be called multiple times to run several transports
  concurrently (e.g., SSE for Claude Desktop + UDS for a cron job).
- **Threading model**: the MCP server runs on a dedicated pthread
  owned by `VmafMcpServer`. The measurement thread is untouched;
  all MCP work (JSON parsing, socket I/O, tool dispatch) happens
  on the MCP thread and enqueues work into a lock-free SPSC queue
  drained at measurement-thread frame boundaries. This keeps the
  NASA Power of 10 no-dynamic-alloc-after-init invariant on the
  hot path.
- **Tool surface (v1)**: read-only introspection + queued hot-swap
  commands.
  - `vmaf.status` — current model, active features, frames
    processed, last score.
  - `vmaf.features` — list of active feature extractors + options.
  - `vmaf.score` — the most recent per-frame + aggregate score.
  - `vmaf.request_model_swap(name)` — enqueues a model swap;
    executes at the next frame boundary if safe, returns
    `MODEL_SWAP_ENQUEUED` otherwise.
  Write / mutating tools beyond model-swap stay out of v1 until
  there's a concrete user need; each new mutating tool gets its
  own follow-up ADR.
- **JSON library**: [`cJSON`](https://github.com/DaveGamble/cJSON)
  vendored under `subprojects/cJSON/`. Single-file, MIT, no deps,
  already used by one of our test helpers. Heavier options
  (jansson, simdjson) give nothing this use case needs.
- **SSE server**: [`mongoose`](https://mongoose.ws/) vendored as a
  single-file dependency. MIT-or-commercial dual-license; we use
  the MIT version. Mongoose is the smallest credible embeddable
  HTTP server that supports SSE out of the box.
- **Lifecycle**: `vmaf_mcp_start` is called by the host (CLI, test
  harness) after `vmaf_init` completes and before the first
  `vmaf_read_pictures`. `vmaf_mcp_stop` is called before
  `vmaf_close`. The server refuses to start after measurement
  has begun; the CLI exposes `--enable-mcp-sse=<port>` and
  `--enable-mcp-uds=<path>` flags to trigger startup at the right
  moment.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Three transports, build-flag-gated (chosen) | Library remains clean by default; each deployment chooses the subset that fits; matches canonical MCP transport options | Largest surface area of the alternatives; three transport backends to maintain | User explicitly selected "All three" in the 2026-04-20 question round; maintenance cost is bounded (each transport is ~300 LOC) |
| SSE only | Smallest v1 footprint; natural library shape; matches Claude Desktop remote-MCP support | Leaves stdio-native tooling (Cursor shells, CI scripts piping JSON) without a path | SSE alone doesn't cover headless / pipe-based workflows |
| UDS only (Linux-only) | Simplest; fits systemd world | Windows / macOS users lose the feature; contradicts the cross-platform backend policy | We don't ship Linux-only features in the core library |
| Thread-queue callback to the existing Python MCP server | Avoids C JSON-RPC parsing | Couples the C library to a Python process; violates the fork's "C library stands alone" principle | Architectural regression |
| Keep MCP external-only | Zero new code | Leaves the "steer a running measurement" workflow unreachable; fails issue #66's explicit ask | User has asked for this and the workflow value is real |

## Consequences

**Positive**

- Unblocks the agentic-VMAF workflow. Agents can introspect and
  (in v1, narrowly) steer measurements.
- Build-flag default-off means no impact on the 99% of consumers
  who don't want MCP — they don't pay for it in code size, startup
  time, or attack surface.
- Three transports give deployment flexibility: SSE for
  desktop-agent users, UDS for headless Linux, stdio for pipe-into-a-subprocess
  cases.
- Clean public header (`libvmaf_mcp.h`) keeps the MCP surface out
  of the core `libvmaf.h` ABI contract.

**Negative**

- New optional dependencies (cJSON, mongoose) in
  `subprojects/`. Both single-file, but still something to track.
- Thread-safety audit cost: the measurement thread must be proven
  safe to share per-feature state with the MCP thread via the SPSC
  queue pattern. Verified by TSan tests in the implementation PR.
- MCP semantics evolve. We may need to re-revisit the transport
  choice if a future MCP version deprecates SSE or adds a new
  transport.
- Three-transport CI: each transport needs its own smoke test. Gate
  only the default-off / off-by-default matrix leg.

**Neutral**

- No change to the Netflix CPU golden gate or any numerical
  output — MCP is an I/O surface, not a measurement surface.
- The existing `mcp-server/vmaf-mcp/` stays as the recommended
  default for "I just want to score a video via an agent"; the
  embedded server is additive.

## Operational guardrails

- **Power of 10 compliance**: no dynamic allocation on the
  measurement thread after `vmaf_mcp_start`. All MCP-thread allocs
  happen before the first frame or in response to agent requests,
  never in the measurement hot loop.
- **No heap allocation in the SPSC queue** — fixed-size ring buffer
  sized at `vmaf_mcp_start`; overflow returns `VMAF_ERR_MCP_QUEUE_FULL`
  to the agent and the measurement thread continues untouched.
- **No unbounded loops** — every MCP tool has an explicit iteration
  cap proved at review time (per Power of 10 rule 2).
- **Authentication**: SSE loopback is bound to `127.0.0.1` only;
  UDS uses filesystem permissions (mode 0700); stdio is trusted by
  construction. No network-accessible MCP server ships in v1.

## References

- [req] AskUserQuestion popup answered 2026-04-20: embedded MCP
  "Yes, build it"; follow-up: "Given that reasoning, final MCP
  transport call?" → "All three (SSE + UDS + stdio)".
- [Research-0005](../research/0005-embedded-mcp-transport.md) —
  design digest: threading model, JSON library selection, SSE
  library selection, Power of 10 compatibility analysis.
- [Anthropic MCP specification](https://modelcontextprotocol.io/spec)
- [mongoose HTTP server](https://mongoose.ws/)
- [cJSON](https://github.com/DaveGamble/cJSON)
- [ADR-0122](0122-cuda-gencode-coverage-and-init-hardening.md) —
  precedent for optional-dependency dlopen loader pattern;
  comparable shape for the MCP sub-flag gating.
- [`mcp-server/vmaf-mcp/`](../../mcp-server/vmaf-mcp/) — the
  external Python MCP server this one complements.
- [CLAUDE.md §12 r10](../../CLAUDE.md) — per-surface docs rule
  (new API header gets a doc entry under
  `docs/api/libvmaf_mcp.md` and `docs/mcp/embedded.md`).

### Status update 2026-05-08: stays Proposed — runtime in flight (T5-2b)

Audited as part of the 2026-05-08 ADR `Proposed` sweep
([Research-0086](../research/0086-adr-proposed-status-sweep-2026-05-08.md)).

The strategic decision in this ADR is shipped in stages:

- The public C-API surface
  (`core/include/libvmaf/libvmaf_mcp.h`) is shipped via the
  audit-first scaffold in [ADR-0209](0209-mcp-embedded-scaffold.md)
  (Accepted).
- Build wiring (`enable_mcp` / `enable_mcp_sse` /
  `enable_mcp_uds` / `enable_mcp_stdio`) is shipped.
- The stub TU at `core/src/mcp/mcp.c` returns `-ENOSYS` for
  every entry point.

What remains unshipped (tracked as backlog item **T5-2b**):

- `cJSON` and `mongoose` vendoring (`subprojects/`).
- Dedicated MCP `pthread` and SPSC ring drain at frame boundaries.
- SSE / UDS / stdio transport bodies.
- Tool-surface expansion past the contract pinned by
  `core/test/test_mcp_smoke.c`.

Because the stub returns `-ENOSYS` rather than the runtime behaviour
the Decision section promised, this ADR stays **Proposed** until
T5-2b lands. ADR-0209 (Accepted) is the audit-first companion — it
explicitly defers the runtime, so it does not supersede this ADR;
both stay live. When T5-2b lands, the closing PR flips this ADR to
Accepted via a follow-up status-update appendix.

Verification command:

```sh
head -25 core/include/libvmaf/libvmaf_mcp.h     # docstring
                                                    # explicitly
                                                    # notes -ENOSYS
head -25 core/src/mcp/mcp.c                     # stub TU
```
