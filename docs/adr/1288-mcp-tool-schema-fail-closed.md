<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1288: An MCP tool schema that fails to marshal is fatal, never defaulted

- **Status**: Accepted
- **Date**: 2026-09-21
- **Deciders**: Lusoris
- **Tags**: `mcp`, `agents`, `security`, `api`

## Context

`cmd/vmafx-mcp/tools.go` builds each tool's JSON-Schema as a Go literal and serialises it at
startup. The original helper, `mustSchema`, panicked on a `json.Marshal` failure. HISS-07
flags a `panic` in a production code path (`.standards-baseline.json` carried
`cmd/vmafx-mcp/tools.go:962:HISS-07`, "Legacy panic invocation in production code path"), so
the HISS-21 burn-down had to discharge it.

The first attempt replaced the panic with a logged fallback to `{"type":"object"}`. That
schema is not a degraded version of the real one — it is the absence of one. JSON-Schema
validation is the server's only check on tool arguments before a handler runs, and an empty
object schema accepts every argument map. The result is a tool that is listed, looks healthy
to the client, and has had its declared contract silently switched off; the Go↔Python parity
tests would not catch it, because they only compare the tools that *are* registered. Trading
a loud startup failure for a silent, permanently-degraded tool is the wrong direction for a
surface that agents call unattended.

## Decision

A tool input schema that fails to marshal aborts registration and the process. `toolSchema`
returns `(json.RawMessage, error)`; `toolRegistrar.add` is the only writer of
`mcp.Tool.InputSchema`, so no caller can construct a tool whose schema did not marshal. The
first failure is retained, every later `add` is a no-op, `registerTools` returns the error,
`buildServer` discards the half-built server, and the fx provider `buildMCPServer` fails the
graph so the process exits non-zero. No default schema is ever substituted, and there is no
flag to relax this — the failure is a defect in the server's own literals, not an operator
condition.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Return the error up the registration chain, fatal at startup (chosen) | No reachable path from a marshal failure to a registered tool; the failure names the tool; satisfies HISS-07 without a `panic` | `buildServer` / `buildMCPServer` grow an error return, and five test call sites check it | — |
| Keep `panic` in `mustSchema` | Simplest; already fail-fast | Violates HISS-07, which is the invariant being discharged; a panic in an fx provider unwinds without fx's shutdown path | Rejected: the rule exists and a panic is the crudest form of the correct behaviour |
| Log and substitute `{"type":"object"}` | Server keeps serving the other 23 tools | The affected tool stays reachable with no argument validation at all, indistinguishable over the wire from a healthy one; a log line on a stdio server is easily lost | Rejected: this is the finding this ADR exists to correct |
| Register the tool but make its handler refuse every call | Keeps the tool list stable | Same lie to the client (listed, advertised, unusable), with a worse error surface — a per-call failure instead of one at startup | Rejected: a broken tool listed as available is worse than a server that refuses to start |
| Marshal all schemas in an `init()` and `log.Fatal` | No signature changes | `log.Fatal` bypasses fx's lifecycle, and `init()`-time failure cannot say which fx provider it belongs to | Rejected: the fx graph already has a failure channel |

## Consequences

- **Positive**: "a listed tool is a validated tool" holds unconditionally. A schema defect
  surfaces at startup with the tool's name instead of as unexplained handler behaviour later.
  The `panic` is gone, so HISS-07 is discharged structurally rather than by suppression.
- **Negative**: one malformed schema literal takes the whole server down, so a defect in one
  tool blocks the other 23. That is the intended trade: the schemas are compile-time literals
  with no runtime input, so the failure is a source bug that must be fixed, not tolerated.
- **Neutral / follow-ups**: `buildServer` returns `(*mcp.Server, error)` — the `cmd/vmafx-mcp/AGENTS.md`
  seam note that previously said "do not change signature" now records why the error return is
  load-bearing. `cmd/vmafx-mcp/tool_schema_test.go` pins each link of the chain, including that
  a marshal failure yields neither a permissive schema nor a registered tool.

## References

- `cmd/vmafx-mcp/AGENTS.md` invariants #19 and the `buildServer` seam.
- `docs/mcp/index.md` § "Startup contract: all tools or none".
- `docs/rebase-notes.md` § "vmafx-mcp tool schemas fail closed (2026-09-21)".
- [ADR-1173](1173-mcp-grpc-bridge-go-only.md) — the Go-only gRPC bridge tools that share this registration path.
- HISS-07 ("Checked Errors") in `.config/hiss/coverage.yaml`; baseline fingerprint `cmd/vmafx-mcp/tools.go:962:HISS-07`.
- Source: adversarial review of branch `chore/hiss21-go-cmd`, paraphrased: a schema that accepts anything is not a safe fallback for a schema that failed to build; "handled" under HISS-07 means propagated or fatal, not swallowed into a permissive default.
