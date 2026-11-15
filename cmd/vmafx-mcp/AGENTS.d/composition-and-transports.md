---
paths:
  - cmd/vmafx-mcp/main.go
  - cmd/vmafx-mcp/server.go
  - cmd/vmafx-mcp/http_security*.go
  - cmd/vmafx-mcp/otel_test.go
  - mcp-server/vmaf-mcp/src/vmaf_mcp/http_transport.py
invariant: fx lifecycle transport; buildServer error seam; stdio stdout pure; HTTP auth/4MiB/loopback parity.
---
<!-- markdownlint-disable MD013 MD029 -->
# fx composition root, stdio purity, HTTP hardening and OTel spans

## Composition root (golusoris fx, ADR-1119 Phase-1 PR-5)

`main.go` = `fx.New(...).Run()` over `internal/app/bootstrap.Base` (`bootstrap.Core` = golusoris config + slog + clock + id + validate, plus `otel.Module`, build-version supply). Mirrors sibling migrations (`cmd/vmafx-server`, `cmd/vmafx-node`). Shape:

```go
fx.New(
    bootstrap.Base,
    fx.Replace(config.Options{EnvPrefix: "VMAFX_", Delimiter: ".", Watch: true}),
    bootstrap.FxLogger(),
    fx.Provide(buildMCPServer),  // (*slog.Logger, *config.Config) -> (*mcp.Server, error); reuses buildServer
    fx.Invoke(runMCPTransport),  // wires the transport in a lifecycle hook
).Run()
```

Facts:

- **MCP server NOT golusoris server module.** golusoris ships no MCP module -> no `bootstrap.HTTP` / `grpc.Module` in graph. Transport (stdio or streamable-HTTP from config) owned in `runMCPTransport` fx lifecycle hook: `OnStart` launches goroutine, `OnStop` drains. If golusoris adds MCP module later, fold hook into module. Do not express transport as framework module.
- **`buildServer` domain seam.** `buildMCPServer` = thin fx provider calling `buildServer(*slog.Logger) (*mcp.Server, error)` (`server.go`). `*config.Config` param exists for fx signature / config-driven wiring; `buildServer` itself needs only logger. Tests call `buildServer(nil)` directly and must check error. Error return load-bearing: only path tool failing schema marshal stops process instead of serving without argument validation (invariant #19). Do not widen signature; do not reduce to single return value.
- **Config keys (env prefix `VMAFX_`, `.` delimiter — `_` becomes `.`):** `VMAFX_MCP_TRANSPORT` -> `mcp.transport` (default `stdio`); `VMAFX_MCP_HTTP_ADDR` -> `mcp.http.addr` (default `:3000`, full listen address). `VMAF_BIN` and `VMAFX_MCP_DIRECT` read directly by tool handlers via `os.Getenv`, NOT koanf — contract unchanged.

## Rebase-sensitive invariants

11. **stdio-stdout purity** (ADR-1119, `main.go`): in stdio mode, `mcp.StdioTransport` owns `os.Stdin` / `os.Stdout` for JSON-RPC framing. Stray write to stdout corrupts stream and breaks IDE MCP clients. fx composition root keeps stdout clean: golusoris `log` writes STDERR, `otel.Module` OTLP-gRPC (no stdout writes; silent no-op when no exporter configured), `bootstrap.FxLogger()` routes fx lifecycle events through slog -> STDERR (NOT fx default printer). Output to stdout (`fx.WithLogger` printing stdout, stdout OTel exporter, bare `fmt.Println` / `os.Stdout.Write` in composition root or provider) MUST be gated off or routed stderr in stdio mode. HTTP transport lacks constraint (TCP), but same providers run in both modes; rule: never write stdout anywhere in package except MCP transport. Verify after composition-root change by driving stdio binary with JSON-RPC `initialize` + `tools/list`; assert stdout carries valid JSON-RPC objects only (logs on stderr).

13. **HTTP transport security parity** (ADR-0967): when `mcp.transport=http`, Go transport (`http_security.go::securityMiddleware` + `applyBindHost`, wired in `main.go::runMCPTransport`) and Python transport (`http_transport.py::_make_security_middleware` + `_resolve_bind_host`) MUST enforce same hardening under same env contract: `VMAFX_MCP_HTTP_TOKEN` (bearer token, constant-time compare — `crypto/subtle.ConstantTimeCompare` ↔ `hmac.compare_digest`), `VMAFX_MCP_HTTP_NO_AUTH=1` (explicit opt-out), **refuse-all 401 when neither set**, **4 MiB** request-body limit (`http.MaxBytesReader` + Content-Length pre-flight -> 413 ↔ `MAX_REQUEST_BODY_BYTES`), loopback-only default bind (`VMAFX_MCP_HTTP_BIND`, default `127.0.0.1`). Client gets same accept/reject decision from either server. `TestSecurityMiddleware*` / `TestApplyBindHost` (Go) and `tests/test_http_transport.py` security block (Python) pin both sides. Do not relax refuse-all default or widen bind default without changing BOTH servers and ADR-0967.

15. **One `vmafx.mcp.tool` span per tool call, from `addRawTool`** (`tools.go`, ADR-0782 / ADR-1119): registration wrapper = binary's only per-request span site on both transports. Tags `AttrMCPTool` with tool name; records parse / handler / marshal failures on span status while response keeps invariant #2 `IsError` contract (never non-nil error to SDK). HTTP transport handler chain: `bootstrap.TraceHTTPHandler(securityMiddleware(mcp handler))` — tracing outermost so 401/413 rejections traced (`main.go`). OTel init from `bootstrap.Base` (no-op without endpoint; stdio purity #11 unaffected because OTel never writes stdout). `otel_test.go::TestToolCallEmitsSpan` and `TestOTelWiredThroughBootstrap` lock this.
