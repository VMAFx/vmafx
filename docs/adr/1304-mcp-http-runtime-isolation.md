<!-- markdownlint-disable MD013 MD060 -->
# ADR-1304: Bind MCP HTTP scoring runtimes per server instance

- **Status**: Proposed
- **Date**: 2026-09-23
- **Deciders**: VMAFx maintainers
- **Tags**: `mcp`, `python`, `architecture`, `concurrency`

## Context

The Python MCP HTTP transport formerly imported scoring helpers from the
canonical stdio server, while the stdio server imported the HTTP transport at
startup. CodeQL alerts 917 and 918 correctly reported that cycle. A narrow
`HttpScoringRuntime` protocol removes the return edge without moving roughly 25
transitive scoring helpers out of their established owner.

The first protocol implementation let a direct `run_http_server(runtime=...)`
caller temporarily replace a process-global runtime. Two overlapping embedded
servers could then observe each other's path-validation and scoring policy,
restore the registry out of order, and leave stale policy installed after both
servers stopped. The import DAG and per-server ownership therefore have to be
decided together.

## Decision

Keep a narrow shared `HttpScoringRuntime` protocol. The canonical server may
install its default adapter once for ordinary CLI startup, but an explicitly
injected adapter is resolved once by `run_http_server` and passed through
`_serve` into the aiohttp application. An injected adapter never mutates the
process-wide registry. Each application captures exactly one adapter for its
entire lifetime.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Hide one import edge behind `importlib` | Small diff | Preserves the real architectural cycle while defeating static analysis, typing, and IDE navigation | A scanner workaround is not a source fix |
| Move scoring and all transitive helpers into a shared implementation module | Direct ownership with no adapter | Moves about 25 helpers, caches, constants, and request types; large regression surface | Disproportionate to the five operations HTTP needs |
| Temporarily replace the process-wide adapter around each embedded server | Minimal call-chain change | Concurrent servers cross-contaminate policy and restore stale global state out of order | Unsafe under supported embedding concurrency |
| Bind the narrow adapter to each server application | Acyclic graph, explicit lifetime, isolated concurrent servers, canonical behavior unchanged | Adds explicit dependency threading through two private functions | Chosen |

## Consequences

- **Positive**: the production import graph is acyclic; concurrent embedded
  servers cannot observe or overwrite each other's scoring runtime; missing
  runtime registration still fails before event-loop allocation or socket bind.
- **Negative**: private transport helpers carry an explicit runtime parameter,
  and embedding adapters must implement four protocol methods.
- **Neutral / follow-ups**: the canonical one-time registry remains for normal
  CLI startup and for direct application construction that omits an adapter.
  Import-graph and runtime-locality regressions are pinned in the MCP tests;
  the default `[dev]` environment includes the HTTP metrics dependency so
  those tests execute in CI instead of being skipped during collection.

## References

- [Research-2028](../research/2028-code-scanning-audit-2026-09-03.md) — alert
  evidence and architectural decision matrix.
- [HTTP transport embedding guide](../mcp/http-transport.md).
- GitHub CodeQL alerts 917 and 918.
- req: "oh of course all bugs.md's in this local repo should of course be fully fixed"
