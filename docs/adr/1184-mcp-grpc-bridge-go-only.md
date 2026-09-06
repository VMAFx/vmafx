<!-- markdownlint-disable MD013 MD060 -->
# ADR-1184: The MCP gRPC control-plane bridge is Go-only

- **Status**: Accepted
- **Date**: 2026-09-05
- **Deciders**: lusoris
- **Tags**: `mcp`, `go`, `grpc`, `agents`, `docs`

## Context

Epic #1240's Priority-3 item asks for five MCP tools over the Phase-4b control
plane — `submit_job`, `get_job`, `cancel_job`, `list_jobs` and
`vmaf_score_remote` — on top of the existing building blocks: `proto/vmafx.proto`,
`cmd/vmafx-controller/proto/controller.proto`, the vendored stubs under `gen/go/`,
and `pkg/score.Client`.

The fork ships two MCP servers. `cmd/vmafx-mcp` (Go) and
`mcp-server/vmaf-mcp` (Python) are held to a parity contract
(`cmd/vmafx-mcp/AGENTS.md` invariant #10): a client must get the same answer from
either one. The contract is enforced as *Go is a superset of Python* —
`server_test.go::TestToolListMatchesPython` asserts that every Python tool exists
in Go, not the converse — and the surface already diverges in the other direction
(the vision-language output of `describe_worst_frames` exists only in Python,
recorded in [ADR-0704](0704-vmafx-mcp-go-port.md)).

Giving the Python server these five tools requires a gRPC client: `grpcio` plus
generated Python stubs for both `.proto` files, regenerated and vendored on every
schema change. That is precisely the dependency Phase 4 is removing — ADR-0704's
stated motivation for the Go port is that the Python wheel chain
"complicates deployment", and `mcp-server/vmaf-mcp/pyproject.toml` keeps its base
install to three packages with everything heavier behind an extra. The
architecture also names the client:
[docs/architecture/phase4b-distributed-platform.md](../architecture/phase4b-distributed-platform.md)
draws `MCP -->|gRPC| CTRL` for the Go `vmafx-mcp` binary and lists it as the
distroless/cc component that "delegates scoring to the controller gRPC API". The
Python server is not in that diagram.

## Decision

We will implement the five control-plane bridge tools in the Go MCP server only
(`cmd/vmafx-mcp/impl_grpc.go`), and leave the Python server without them. The
parity contract is amended to say so explicitly: the sidecar-binary tools
(`vmaf_per_shot`, `vmaf_roi`, `vmaf_bench`, `vmaf_vpl`) are byte-compatible twins
on both servers, and the gRPC bridge is a documented Go-only category. Connection
targets and credentials come from the environment (`VMAFX_CONTROLLER_ADDR`,
`VMAFX_SERVER_ADDR`, `VMAFX_CONTROLLER_TOKEN`, `VMAFX_GRPC_TIMEOUT`), never from
a tool argument.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Go-only bridge (chosen)** | No new Python dependency; matches the Phase-4b diagram; the existing superset parity test already permits it | The two servers' tool lists differ; a Python-only deployment cannot submit jobs | The divergence is documented and one-directional, and it is the client the architecture actually names |
| Add `grpcio` + vendored Python stubs to `mcp-server/vmaf-mcp` | Full tool parity | Re-imports the wheel chain ADR-0704 exists to remove; stubs for two `.proto` files must be regenerated and vendored on every schema change; doubles the surface every future control-plane RPC has to land on | Directly contradicts the Phase-4 language-consolidation direction |
| Python shells out to a small Go client binary | No Python gRPC dependency | No such client binary exists; adding one means a new `cmd/` target, build wiring, release artifact and install path for a server that is being sunset | Cost is a new shipped binary, for a server on the ADR-0704 Stage-2 sunset path |
| Python calls the controller's HTTP API | Reuses `aiohttp`, already an optional extra | The controller's HTTP surface exposes only `/v1/score`, `/healthz`, `/readyz` and `/metrics` — there are no job endpoints, so this means designing and building a second control-plane API | Would fork the control-plane API surface to serve the legacy server |
| Defer Priority-3 entirely | Smallest diff | Leaves the epic's largest remaining item open and the building blocks (proto, stubs, `pkg/score.Client`) unused | The Go half is complete and verified end-to-end against a running controller |

## Consequences

- **Positive**: the control-plane bridge lands now, against the client the
  architecture prescribes, with no new dependency in either server. The Python
  server's base install stays at three packages.
- **Negative**: the two servers' tool lists are no longer identical. An operator
  running only the Python server cannot submit or inspect controller jobs and
  must switch to `vmafx-mcp`. [docs/mcp/tools.md](../mcp/tools.md) marks each of
  the five tools **Go only**.
- **Neutral / follow-ups**: `cmd/vmafx-mcp/AGENTS.md` invariant #10 is amended and
  a new invariant #15 records the sidecar parity requirement, so a future agent
  does not "restore parity" by deleting the Go-only tools. If the Python server
  outlives the ADR-0704 Stage-2 sunset, revisit the `grpcio` option rather than
  removing the tools.

## References

- Epic #1240, Priority-3 item: tools `submit_job`, `get_job`, `cancel_job`,
  `list_jobs`, `vmaf_score_remote` on top of the existing proto / stub /
  `pkg/score/grpc_client.go` building blocks; the task brief left the Python-side
  design open ("the Python side may call the Go server over gRPC (grpcio) or shell
  out to a Go client - pick the design the docs prescribe and say why").
- [ADR-0704](0704-vmafx-mcp-go-port.md) — the Go port and the Python-wheel-chain motivation.
- [ADR-0711](0711-vmafx-controller-impl.md) — the controller Client API.
- [ADR-0703](0703-vmafx-server-go-grpc.md) — `vmafx-server` gRPC + HTTP.
- [ADR-0962](0962-controller-streamjobs-and-reaper-stop.md) — `StreamJobs` streams a snapshot and closes, which is what `list_jobs` drains.
- [docs/architecture/phase4b-distributed-platform.md](../architecture/phase4b-distributed-platform.md) — `MCP -->|gRPC| CTRL`.
