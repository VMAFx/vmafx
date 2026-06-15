- **`vmafx-mcp` migrated onto the golusoris fx framework** (ADR-1119,
  Phase-1 PR-5). The hand-rolled composition root (`flag.Parse` +
  `signal.NotifyContext` + bespoke stdio/HTTP transport lifecycles + custom
  OTel init + logger) is replaced by an `fx.New(...).Run()` over golusoris
  modules: `golusoris.Core` (config + structured slog logging on stderr) and
  `otel.Module`, via the shared `internal/app/bootstrap.Base`. golusoris has
  no MCP server module, so the MCP transport (stdio or streamable-HTTP, per
  config) is wired directly in an fx lifecycle hook (`runMCPTransport`):
  `OnStart` launches the transport, `OnStop` drains it. The entire MCP tool
  surface (`buildServer` / `tools.go` / `impl.go` / `impl_direct.go`) is
  unchanged — the Go↔Python byte-identical scoring schema and `vmaf` CLI argv
  (cmd/vmafx-mcp/AGENTS.md invariant #10) are untouched, and `serverInfo`
  stays `{"name":"vmafx-mcp","version":"1.0.0"}`.
- **stdio-stdout purity preserved.** The stdio transport owns stdin/stdout for
  the JSON-RPC framing; golusoris log writes to stderr, `otel.Module` is
  OTLP-gRPC (no stdout writes; a silent no-op when no exporter is configured),
  and `bootstrap.FxLogger()` routes fx's lifecycle events through slog →
  stderr. Verified empirically: in stdio mode stdout carries only valid
  JSON-RPC responses; all framework logging goes to stderr.
- **Breaking — MCP transport-selection contract.** The `--transport`
  (`stdio`|`http`) and `--port` (int, default `3000`) CLI flags are removed
  (golusoris config is env-driven). The equivalents are the env vars
  `VMAFX_MCP_TRANSPORT` (koanf `mcp.transport`, default `stdio`) and
  `VMAFX_MCP_HTTP_ADDR` (koanf `mcp.http.addr`, default `:3000` — a full
  listen address, not a bare port; the historical port 3000 is preserved as
  the default address). Operators using `--transport http --port N` must
  migrate to `VMAFX_MCP_TRANSPORT=http VMAFX_MCP_HTTP_ADDR=:N`. The
  `VMAF_BIN` and `VMAFX_MCP_DIRECT` env contracts (read directly by the tool
  handlers, not via koanf) are unchanged.
