<!-- markdownlint-disable MD013 MD060 -->
# ADR-0209: Embedded MCP server — scaffold-only audit-first PR (T5-2)

- **Status**: Accepted
- **Date**: 2026-04-29
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: mcp, agents, api, scaffold, audit-first, fork-local

## Context

[ADR-0128](0128-embedded-mcp-in-libvmaf.md) (Proposed, 2026-04-20)
decided the fork would embed a Model Context Protocol (MCP)
server inside `libvmaf.so` with three transports — SSE, Unix
domain socket, and stdio — gated behind a build flag. The ADR
sketched the threading model (dedicated MCP pthread plus an SPSC
ring drained at frame boundaries), the JSON library
(`cJSON`), and the SSE library (`mongoose`). What it deliberately
deferred: how to land that without a single mega-PR.
[Research-0005](../research/0005-embedded-mcp-transport.md) §
"Next steps" enumerated the implementation sequence: skeleton
PR → SSE transport PR → UDS transport PR → stdio transport PR →
tool-surface expansion → docs.

This ADR is the audit-first companion. Same shape as
[ADR-0175](0175-vulkan-backend-scaffold.md) for the Vulkan
backend (T5-1), [ADR-0184](0184-vulkan-image-import-scaffold.md)
for VkImage zero-copy import (T7-29 part 1), and
[ADR-0173](0173-ptq-int8-audit-impl.md) for the PTQ harness:
ship the **static surfaces** (public header, build wiring,
stub TU, smoke test, docs) in a focused PR so the runtime PR
that follows has a stable base to land on.

The "Tiny MCP inside libvmaf" workflow is a different shape than
the Python MCP server already shipping under `mcp-server/vmaf-mcp/`
(see [ADR-0009](0009-mcp-server-tool-surface.md),
[ADR-0166](0166-mcp-server-release-channel.md),
[ADR-0172](0172-mcp-describe-worst-frames.md)). The Python
server wraps the `vmaf` CLI and is the right answer when the agent
just wants to score a video. The embedded server lets agents
**steer a running measurement** — hot-swap models mid-stream,
query per-frame state during long sweeps, pause/resume on external
signals — none of which fit a CLI-wrapping process.

## Decision

### Land scaffold only — no transport runtime, no JSON library, no SSE library

The PR creates:

- Public header
  [`core/include/libvmaf/libvmaf_mcp.h`](../../core/include/libvmaf/libvmaf_mcp.h):
  declares `VmafMcpServer`, `VmafMcpConfig`, `VmafMcpSseConfig`,
  `VmafMcpUdsConfig`, `VmafMcpStdioConfig`, `VmafMcpTransport`;
  entry points `vmaf_mcp_available`,
  `vmaf_mcp_transport_available`, `vmaf_mcp_init`,
  `vmaf_mcp_start_sse`, `vmaf_mcp_start_uds`,
  `vmaf_mcp_start_stdio`, `vmaf_mcp_stop`, `vmaf_mcp_close`. Pure
  C99 — no `<vulkan/vulkan.h>`-style transitive includes; the
  opaque server handle is forward-declared. Mirrors the
  CUDA/SYCL/Vulkan public-header pattern.
- Stub TU at
  [`core/src/mcp/mcp.c`](../../core/src/mcp/mcp.c) — every
  public entry point validates its arguments (returns `-EINVAL`
  for NULLs) then returns `-ENOSYS` (or 0 / no-op for `_stop` /
  `_close`). NASA Power-of-10 rule 7 satisfied: every non-void
  return is checked or `(void)`-cast at every call site (the TU
  itself has no callees beyond `errno.h` macros).
- Build wiring: new umbrella `enable_mcp` (boolean, default
  **false**) plus per-transport sub-flags `enable_mcp_sse`,
  `enable_mcp_uds`, `enable_mcp_stdio` (boolean, default
  **false**) in
  [`core/meson_options.txt`](../../core/meson_options.txt).
  Conditional `subdir('mcp')` in
  [`core/src/meson.build`](../../core/src/meson.build);
  `mcp_sources` + `mcp_defines` threaded through the `library()`
  call alongside the existing `dnn_sources` aggregation.
- Smoke test
  [`core/test/test_mcp_smoke.c`](../../core/test/test_mcp_smoke.c)
  with 12 sub-tests pinning the scaffold contract
  (availability, NULL guards on every entry point, the `-ENOSYS`
  body, idempotent close). Wired in
  [`core/test/meson.build`](../../core/test/meson.build)
  under `if get_option('enable_mcp')`.
- New docs at
  [`docs/mcp/embedded.md`](../mcp/embedded.md) — design summary,
  build flag matrix, status table, follow-up roadmap to T5-2b.

### Zero runtime dependencies for the scaffold

The scaffold has no `dependency('cjson')`, no
`subprojects/cJSON/`, no `subprojects/mongoose/`, no `pthread`
beyond what the test harness already pulls in. Adding those is
the responsibility of the T5-2b runtime PR. Reasoning: matches
the ADR-0175 / ADR-0184 audit-first precedent — the scaffold's
CI run validates "the build wiring + meson dispatch + test
harness work end-to-end"; landing a vendored cJSON + mongoose
before any code uses them gates the scaffold's CI green-light on
single-file deps that are dead weight until the runtime PR.

### All flags default `false`

Both the umbrella `enable_mcp` and the per-transport sub-flags
default off. `auto` would silently flip on in distro builds; for
a brand-new server surface that returns `-ENOSYS` everywhere, a
silent flip would mean release-mode `libvmaf.so` carries the new
public symbols but every call fails — confusing for downstream
consumers compiling against the header and seeing it "work" at
link time. Operators who want the scaffold smoke test to run
flip `-Denable_mcp=true` explicitly.

### Per-transport sub-flags from day one

Per ADR-0128 § "Per-transport sub-flags", the build accepts
`-Denable_mcp_sse=true` etc. independently. Each maps to a
preprocessor define (`HAVE_MCP_SSE`, `HAVE_MCP_UDS`,
`HAVE_MCP_STDIO`) the runtime PR will key its transport-specific
TUs off. Keeping the sub-flag wiring in this scaffold means the
T5-2b PR can land transport bodies one at a time without ever
touching `meson_options.txt`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Audit-first scaffold (chosen)** | Header surface stable for downstream consumers from day one; T5-2b runtime PR lands against a green base; review skill split (build wiring vs runtime correctness vs JSON-RPC contract) matches the ADR-0175 / ADR-0184 / ADR-0173 precedent | Three new files + four edited meson files + an ADR + a doc, no runtime functionality yet | Lowest-risk path; matches the established fork pattern for L-sized backlog items |
| **Land scaffold + cJSON + mongoose vendoring + first transport in one PR** | Single PR closes T5-2 and delivers a usable transport | Review burden mixes "is the build flag right" with "is the SSE event-loop correct" with "is the JSON-RPC contract spec-conformant"; rejected by the ADR-0175 precedent for the same reason | Too large to review in one pass |
| **Single transport scaffold (e.g. SSE-only) per PR, deferring UDS / stdio sub-flags** | Smallest possible scaffold | The user popup answer in the 2026-04-20 round was "All three" (per ADR-0128 § References). Splitting the build flags across PRs means three rounds of `meson_options.txt` edits and re-reviewing the same flag pattern three times | User direction is explicit; fork pays the build-wiring cost once |
| **stdio-first instead of SSE-first as the audit pivot** | stdio is simpler than SSE (no HTTP framing, no port allocation) | The header surface this scaffold pins is identical regardless of which transport runtime lands first; the ordering is a T5-2b decision, not a T5-2 one | Out of scope for the scaffold |
| **No umbrella flag — only per-transport flags** | Slightly simpler `meson_options.txt` | The umbrella `enable_mcp` flag is what `enable_dnn` / `enable_vulkan` already do; downstream `pkg-config` / FFmpeg `--enable-libvmaf-mcp` configure probes are simpler when there's a single canonical "is the embedded MCP surface present" symbol to check | Established fork convention wins |
| **Default `enable_mcp` to `auto`** | Pick up MCP whenever the toolchain is present | The scaffold has no toolchain dependencies — `auto` would always resolve to `enabled`, silently shipping `-ENOSYS` symbols in release-mode libvmaf.so. Same silent-flip rejection as ADR-0175 | Default off; flip to `auto` post-T5-2b once a real transport is wired |

## Consequences

**Positive:**

- Public header lands without committing to runtime details.
  Downstream consumers (FFmpeg filters, third-party agents) can
  compile against `libvmaf_mcp.h` today; `vmaf_mcp_init` returns
  `-ENOSYS` until T5-2b, signalling clearly that the runtime is
  not yet wired.
- Build matrix gains a new lane (`enable_mcp=true`) that compiles
  the scaffold on every PR — bit-rot is caught immediately.
- The 12-subtest smoke pins every public entry point's NULL-guard
  and `-ENOSYS` contract; a future runtime PR that accidentally
  enables a path without flipping smoke expectations trips the
  gate rather than landing silently broken.
- T5-2b can land transport bodies one at a time without touching
  the build flag layout.

**Negative:**

- One public header + one stub TU + one meson subdir + one smoke
  test + one ADR + one doc with no functional code yet.
  Acceptable for an audit-first PR; T5-2b will swap the stub TU's
  bodies in place.
- `vmaf_mcp_available()` returns `1` when built with
  `-Denable_mcp=true` regardless of whether transports are wired.
  Same trade-off the Vulkan scaffold made (ADR-0175 §
  "Consequences"). The function honestly reports "the build was
  opted in"; operators read `docs/mcp/embedded.md` for status.
- No `ffmpeg-patches/` change in this PR — the embedded MCP
  server doesn't probe through `pkg-config --cflags libvmaf` from
  any FFmpeg filter (it's a runtime spawn from the host, not a
  link-time consumer). CLAUDE.md §12 r14 applies only to
  surfaces probed by patches; the embedded server is opt-in via
  CLI / library API, never via filter init.

**Neutral:**

- No change to the Netflix CPU golden gate or any numerical
  output — MCP is an I/O surface, not a measurement surface.
- The existing `mcp-server/vmaf-mcp/` Python server stays
  unchanged; the embedded server is additive per ADR-0128 §
  "Consequences — Neutral".

## Tests

- `core/test/test_mcp_smoke.c` (12 sub-tests, all pass
  locally on the worktree CPU build):
  - `test_available_returns_one`
  - `test_transport_available_unknown_id_is_zero`
  - `test_init_rejects_null_out`
  - `test_init_rejects_null_ctx`
  - `test_init_returns_enosys_until_runtime`
  - `test_start_sse_rejects_null_server`
  - `test_start_uds_rejects_null_path`
  - `test_start_uds_rejects_null_cfg`
  - `test_start_stdio_rejects_negative_fd`
  - `test_stop_rejects_null`
  - `test_close_null_is_noop`
  - `test_close_pointer_to_null_is_noop`
- Local gate: `meson setup build-cpu -Denable_cuda=false -Denable_sycl=false -Denable_mcp=false`
  → 37/37 tests pass; `meson setup --reconfigure -Denable_mcp=true -Denable_mcp_sse=true
  -Denable_mcp_uds=true -Denable_mcp_stdio=true` → 38/38 tests pass (the smoke test is the
  delta).

## What lands next (T5-2b roadmap, per Research-0005 § "Next steps")

1. **Runtime PR**: vendor cJSON under `subprojects/cJSON/` and
   mongoose under `subprojects/mongoose/`. Implement
   `vmaf_mcp_init` (SPSC ring allocation, MCP-pthread creation),
   `vmaf_mcp_stop` (thread join), `vmaf_mcp_close` (handle
   release). Smoke test contract shifts from "_init returns
   -ENOSYS" to "_init succeeds, _close releases cleanly".
2. **SSE transport PR**: `vmaf_mcp_start_sse` body —
   loopback-bound mongoose server, end-to-end `vmaf.status` over
   SSE against a curl harness.
3. **UDS transport PR**: `vmaf_mcp_start_uds` body —
   newline-delimited JSON-RPC, `socat` smoke.
4. **stdio transport PR**: `vmaf_mcp_start_stdio` body —
   LSP-framed JSON-RPC on caller-supplied fd pair.
5. **Tool-surface expansion PR**: first mutating tool
   (`vmaf.request_model_swap`) — separate PR so the hot-swap
   atomic is auditable on its own.
6. **`enable_mcp` default flip** to `auto`: pick up MCP whenever
   the build host has the toolchain, only after the matrix
   proves all three transports stable.

### Status update 2026-05-08: MCP runtime stdio + 2 tools landed (T5-2b v1)

The scaffold's `-ENOSYS` body has been replaced with a working
v1 stdio runtime (HP-4 from the Phase-A audit). Delta:

- **Vendored cJSON v1.7.18** (MIT) under
  `core/src/mcp/3rdparty/cJSON/` — single `cJSON.c`/`cJSON.h`
  pair, ~3,400 LOC; LICENSE preserved verbatim.
- **JSON-RPC 2.0 dispatcher** at `core/src/mcp/dispatcher.c`
  routing `initialize`, `tools/list`, `tools/call`,
  `resources/list`. Method-not-found returns `-32601` envelope.
- **stdio transport** at `core/src/mcp/transport_stdio.c` —
  one dedicated MCP pthread, line-delimited JSON-RPC framing,
  64 KiB per-line cap (NASA Power-of-10 rule 2).
- **Tools shipped**: `list_features` (real — walks the canonical
  feature-extractor list), `compute_vmaf` (placeholder —
  validates `reference_path` / `distorted_path` and returns a
  deferred-to-v2 marker; the real scoring binding lands in v2).
- **Smoke test** flipped from pinning `-ENOSYS` to driving real
  JSON-RPC round-trips over a `pipe(2)` pair: 15 sub-tests, all
  green.

Trimmed (now scoped to v2):

- mongoose vendoring + SSE transport body (`vmaf_mcp_start_sse`
  still returns `-ENOSYS`).
- UDS transport body (`vmaf_mcp_start_uds` still returns
  `-ENOSYS`).
- LSP-framed stdio (`Content-Length:` headers) — v1 ships
  line-delimited only.
- SPSC ring drain at frame boundaries — v1 dispatcher runs to
  completion on the transport thread; the measurement-thread
  hot path is not yet bridged.
- `compute_vmaf` binding to `vmaf_score_pooled()`.

The audit-first / runtime split documented in the original
Decision still holds: this update lands the v1 minimum
(stdio + 2 tools) so the runtime exists, with each remaining
transport body landing as its own PR.

## References

- [ADR-0128](0128-embedded-mcp-in-libvmaf.md) — the governance
  decision this ADR implements (audit-first half).
- [Research-0005](../research/0005-embedded-mcp-transport.md) —
  design digest: threading model, JSON library selection, SSE
  library selection, Power-of-10 compatibility analysis. Already
  covers T5-2's scope; this ADR cites rather than supplements.
- [ADR-0175](0175-vulkan-backend-scaffold.md) — the
  audit-first pattern this ADR follows.
- [ADR-0184](0184-vulkan-image-import-scaffold.md) — same
  audit-first pattern applied to a public-header surface.
- [ADR-0173](0173-ptq-int8-audit-impl.md) — same two-layer
  audit pattern applied to a tooling surface.
- [`mcp-server/vmaf-mcp/`](../../mcp-server/vmaf-mcp/) — the
  external Python MCP server this one complements.
- [BACKLOG T5-2](../../.workingdir2/BACKLOG.md) — backlog row.
- `req` — backlog T5-2 ("Embedded MCP skeleton (SSE + UDS +
  stdio). New `libvmaf_mcp.h` header. Dedicated MCP pthread +
  SPSC ring buffer; no alloc on hot path (Power-of-10 §3).
  Per-transport build flags.") + ADR-0128 § References user
  popup answer 2026-04-20 ("All three (SSE + UDS + stdio)").
