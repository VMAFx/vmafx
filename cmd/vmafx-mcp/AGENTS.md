<!-- GENERATED (`make docs-fragments-write`); do not hand-edit. -->
<!-- markdownlint-disable MD013 -->
# AGENTS.md — cmd/vmafx-mcp

Generated index. Match touched paths against `Touching`; read each matching
page first (none -> skip). Text above table binds every file. `Touching`
relative; leading `/` = repo root. New invariant -> `AGENTS.d/` page
([how](../../docs/development/agents-index.md)).

Go MCP server: exposes 24 tools (`vmaf_score`, `list_models`, ...) to MCP clients (Claude Desktop, Cursor, in-tree gRPC server). Wraps libvmaf C library via 2 paths: legacy `exec.Command(vmaf, ...)` subprocess path (default), direct cgo path from ADR-0931 (opt-in via `VMAFX_MCP_DIRECT=1`).

## Topic pages

| Touching | Read first | Invariant |
| --- | --- | --- |
| `main.go`, `server.go`, `http_security*.go`, `otel_test.go`, `/mcp-server/vmaf-mcp/src/vmaf_mcp/http_transport.py` | [composition-and-transports](AGENTS.d/composition-and-transports.md) | fx lifecycle transport; buildServer error seam; stdio stdout pure; HTTP auth/4MiB/loopback parity. |
| `impl.go`, `impl_direct*.go`, `/pkg/libvmaf/*.go` | [direct-cgo-dispatch](AGENTS.d/direct-cgo-dispatch.md) | Dispatch via directPathEnabled (exact "1"); one stderr marker; additive keys; 4 model arg forms. |
| `impl_grpc*.go`, `tools.go` | [grpc-bridge](AGENTS.d/grpc-bridge.md) | 5 control-plane gRPC tools Go-only in goOnlyTools; env-only targets; validateRemotePath guard. |
| `tools.go`, `impl.go`, `parity_argv_test.go`, `score_extras_test.go`, `/pkg/scoreopts/*.go`, `/mcp-server/vmaf-mcp/src/vmaf_mcp/server.py` | [scoring-and-probe-parity](AGENTS.d/scoring-and-probe-parity.md) | Generated options.gen.json schema/argv; legacy %.6f default; 64x64 probe; CLI backend_used receipt. |
| `impl_sidecar*.go`, `sidecar_parity_test.go`, `/pkg/libvmaf/paths.go`, `/mcp-server/vmaf-mcp/src/vmaf_mcp/server.py` | [sidecar-tools](AGENTS.d/sidecar-tools.md) | Byte-identical sidecar argv + FormatFloat/-1; resolve binaries via libvmaf.FindSidecarBinary. |
| `impl.go`, `impl_gosec_test.go`, `impl_handlers_test.go` | [subprocess-and-gosec](AGENTS.d/subprocess-and-gosec.md) | Validate paths/binaries for gosec G204/G304; >=1 error test per handler; exec.CommandContext. |
| `tools.go`, `tool_contract_test.go`, `tool_schema_test.go`, `/mcp-server/vmaf-mcp/tool-contract.json` | [tool-contract-and-registration](AGENTS.d/tool-contract-and-registration.md) | Match Python tool-contract.json; register via registerTools helpers; fail closed on bad schema. |
