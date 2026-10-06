<!-- markdownlint-disable MD013 -->
# AGENTS.md — cmd/vmafx-mcp

Go MCP server: exposes 24 tools (`vmaf_score`, `list_models`, ...) to MCP clients (Claude Desktop, Cursor, in-tree gRPC server). Wraps libvmaf C library via 2 paths: legacy `exec.Command(vmaf, ...)` subprocess path (default), direct cgo path from ADR-0931 (opt-in via `VMAFX_MCP_DIRECT=1`).

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
- **Interim env bridge.** `main()` bridges `VMAFX_LOG_LEVEL → LOG_LEVEL` and `VMAFX_LOG_FORMAT → LOG_FORMAT` before `fx.New` (golusoris#234, v0.4.0 log module reads bare `LOG_LEVEL`). Delete lockstep with sibling binaries once carrying golusoris tag lands.

## Rebase-sensitive invariants

1. **Tool name + schema parity with Python** (`tools.go`): every tool of Python `_list_tools()` served with same property names, JSON types, required set. Python list = `mcp-server/vmaf-mcp/tool-contract.json`, written by `python3 -m vmaf_mcp.tool_contract --write`, kept current by `mcp-server/vmaf-mcp/tests/test_tool_contract.py`; never hand-edit it, never copy the list into Go. Served tool outside contract = must be in `goOnlyTools` (invariant 17). Enforced by `tool_contract_test.go::TestToolListMatchesPython`, `TestToolSchemasMatchPython`; `TestToolContractComparisonRefusesDrift` plants missing / extra / required / type drift. `.github/ci-impact.json` routes contract changes to Go checks.

2. **Direct/subprocess dispatcher** (`impl.go`, `impl_direct.go`): tool handlers with both paths MUST dispatch via:
   `if directPathEnabled() { runFooDirect(...) } else { runFoo(...) }`.
   Direct variant responsible for fallback to subprocess variant on unhandled cases (GPU backends, `.onnx` models, unresolvable model versions in Phase 1). Keeps env gate safe globally. See ADR-0931 §"Fallback flag" and `docs/architecture/mcp-cgo-direct-migration.md`.

3. **`VMAFX_MCP_DIRECT` strict** (`impl_direct.go::directPathEnabled`): only exact string `"1"` enables direct path. `"true"`, `"yes"`, `"on"` treated as off. Phase 3 may relax; until then strict check asserted by `impl_direct_test.go::TestDirectPathEnabled`.

4. **Marker stream** (`pkg/libvmaf.LogDirectPathSelected`): "VMAFX_MCP_DIRECT=1 ... direct cgo scoring path" marker writes stderr exactly once per process. Operators rely on marker confirming path took effect. Routing through slog or stdout breaks convention.

5. **Response shape additions additive only**: direct path adds `backend_used = "cpu (direct cgo)"` and `frame_count` to subprocess JSON shape. Phase 2/3 populates `frames` and per-feature pooled scores. Existing keys (`pooled_metrics.vmaf.mean`, `backend_requested`, `mismatched_model_warning`) MUST remain identical to subprocess output.

6. **Model arg compatibility** (`impl_direct.go::resolveModelArgToPath`): accepts 4 MCP-level model forms (`version=NAME`, `path=ABS`, bare stem, abs/rel path). New forms require coordinated update to Python server resolver (`mcp-server/vmaf-mcp/src/vmaf_mcp/`).

7. **gosec G304 / G204 contract** (`os.ReadFile` / `os.Open` / `exec.Command*` in `impl.go`): path or command variable consumed MUST either (a) round-trip through `libvmaf.ValidatePath` (caller-supplied paths), (b) originate from `os.CreateTemp` / `os.MkdirTemp` (locally-generated temp paths), or (c) come from `libvmaf.FindBinary` / `findVmafTune` / `exec.LookPath` fixed binary name. Subprocess site or file read without gate = attacker-influenced path; add validation or annotate `// #nosec G204` / `// #nosec G304` with citation naming protecting helper. CI gate (`gosec -exclude-generated` in `go-ci.yml`) blocks merge. `cmd/vmafx-mcp/impl_gosec_test.go::TestDescribeModelRejectsTraversal` pins `describeModel` allowlist; equivalent regressions belong next to it. See [ADR-0983](../../docs/adr/0983-gosec-findings-fix-sweep.md).

8. **Handler test coverage** (`impl_handlers_test.go`): covers "binary not found" fast-fail branches, `findVmafTune` env-override, `parseArgs` nil/valid/invalid, ambiguous model-stem path in `describeModel`, and filesystem walks for `handleListModels` and `handleListExtractors`. When adding handler: add >=1 error-path test not requiring external binary (`t.Setenv("VMAF_BIN", "/nonexistent/...")`). Statement coverage target: 50 %+ on `cmd/vmafx-mcp` package.

9. **Context propagation invariant** (ADR-1085): subprocesses launched from tool handler MUST use `exec.CommandContext(ctx, ...)` where `ctx` comes from MCP dispatch layer. `exec.Command(...)` without context forbidden: leaves orphan processes when MCP client disconnects. Only exception `probeBackends` (uses fresh `context.WithTimeout` for module-level background cache fill, not per-request call). `runVmafScore`, `delegateToPythonEval`, and `runVmafScoreDirect` carry `ctx context.Context` parameter; new subprocess functions must follow same pattern.

10. **Go↔Python byte-identical scoring surface** (ADR-1117 / #1240): Go server (`tools.go` `scoringExtraProperties()` + `impl.go` `parseScoreExtras` / `buildVmafArgv`) and Python server (`server.py` `_scoring_extra_properties()` + `_extras_from_args` / `_build_vmaf_argv`) MUST declare same `vmaf_score` / `vmaf_score_encoded` input schema (property names, types, enums, defaults, required-ness, device selectors `--cpumask`, `--gpumask`, `--sycl_device`, `--hip_device`, `--metal_device`, `output_fmt`, `subsample`, tiny-AI flags) AND build same `vmaf` CLI argv. Client gets same result from either server — 1 exception in invariant #15. Changing scoring param requires updating BOTH sides and keeping canonical argv ORDER identical (`--subsample` only when `>1`, emitted before extras). `TestGoAndPythonArgvParity` (Go) and `test_parity_argv.py` (Python) enforce byte-identical CLI invocation; `score_extras_test.go` and `tests/test_score_extras_adr1117.py` pin schema and bounds validation against `core/tools/cli_parse.c`. Allowed roots must include `python/test/resource/yuv` in both servers so worktree symlinks resolve.

11. **stdio-stdout purity** (ADR-1119, `main.go`): in stdio mode, `mcp.StdioTransport` owns `os.Stdin` / `os.Stdout` for JSON-RPC framing. Stray write to stdout corrupts stream and breaks IDE MCP clients. fx composition root keeps stdout clean: golusoris `log` writes STDERR, `otel.Module` OTLP-gRPC (no stdout writes; silent no-op when no exporter configured), `bootstrap.FxLogger()` routes fx lifecycle events through slog -> STDERR (NOT fx default printer). Output to stdout (`fx.WithLogger` printing stdout, stdout OTel exporter, bare `fmt.Println` / `os.Stdout.Write` in composition root or provider) MUST be gated off or routed stderr in stdio mode. HTTP transport lacks constraint (TCP), but same providers run in both modes; rule: never write stdout anywhere in package except MCP transport. Verify after composition-root change by driving stdio binary with JSON-RPC `initialize` + `tools/list`; assert stdout carries valid JSON-RPC objects only (logs on stderr).

12. **probe_backend parity invariant** (ADR-0608 follow-up): Go `handleProbeBackend` (`impl.go`) and Python `_probe_backend` (`server.py`) MUST use same synthetic probe frame and `runtime_healthy` predicate. Frame = **64x64** 4:2:0 8-bit mid-grey (`probeYUVWidth`/`probeYUVHeight` ↔ `_PROBE_YUV_WIDTH`/`_PROBE_YUV_HEIGHT`): must NOT shrink below 36px per dimension (CUDA ADM kernel returns null score under minimum; naive `runtime_healthy=true` misreports healthy backend). `runtime_healthy` = true iff subprocess exits 0 AND pooled `vmaf.mean` score non-null (Go additionally rejects non-finite floats); on null/non-finite score both servers set `runtime_healthy=false` with error string `"vmaf returned exit 0 but score was null"`. `TestProbeYUVDimensions` / `TestScoreIsHealthy` (Go) and `tests/test_probe_backend_pr850.py` (Python) pin both sides.

13. **HTTP transport security parity** (ADR-0967): when `mcp.transport=http`, Go transport (`http_security.go::securityMiddleware` + `applyBindHost`, wired in `main.go::runMCPTransport`) and Python transport (`http_transport.py::_make_security_middleware` + `_resolve_bind_host`) MUST enforce same hardening under same env contract: `VMAFX_MCP_HTTP_TOKEN` (bearer token, constant-time compare — `crypto/subtle.ConstantTimeCompare` ↔ `hmac.compare_digest`), `VMAFX_MCP_HTTP_NO_AUTH=1` (explicit opt-out), **refuse-all 401 when neither set**, **4 MiB** request-body limit (`http.MaxBytesReader` + Content-Length pre-flight -> 413 ↔ `MAX_REQUEST_BODY_BYTES`), loopback-only default bind (`VMAFX_MCP_HTTP_BIND`, default `127.0.0.1`). Client gets same accept/reject decision from either server. `TestSecurityMiddleware*` / `TestApplyBindHost` (Go) and `tests/test_http_transport.py` security block (Python) pin both sides. Do not relax refuse-all default or widen bind default without changing BOTH servers and ADR-0967.

14. **Score-precision default parity** (ADR-0119 / ADR-1117): every MCP scoring path — Go stdio/subprocess (`impl.go`), Go direct-cgo fallback (`impl_direct.go`), Python stdio (`server.py`), Python HTTP `/v1/score` (`http_transport.py`) — MUST default `precision` arg to `legacy` (`%.6f`, documented C-CLI default). Do not reintroduce `"17"` default on any path: client must get same numeric format regardless of server / transport / dispatch path.

15. **One `vmafx.mcp.tool` span per tool call, from `addRawTool`** (`tools.go`, ADR-0782 / ADR-1119): registration wrapper = binary's only per-request span site on both transports. Tags `AttrMCPTool` with tool name; records parse / handler / marshal failures on span status while response keeps invariant #2 `IsError` contract (never non-nil error to SDK). HTTP transport handler chain: `bootstrap.TraceHTTPHandler(securityMiddleware(mcp handler))` — tracing outermost so 401/413 rejections traced (`main.go`). OTel init from `bootstrap.Base` (no-op without endpoint; stdio purity #11 unaffected because OTel never writes stdout). `otel_test.go::TestToolCallEmitsSpan` and `TestOTelWiredThroughBootstrap` lock this.

16. **Sidecar parity required** (ADR-1184, #1240). 15 classic tools and 4 sidecar tools (`vmaf_per_shot`, `vmaf_roi`, `vmaf_bench`, `vmaf_vpl`) have byte-compatible Python twins. `impl_sidecar.go` `buildPerShotArgv` / `buildRoiArgv` / `buildBenchArgv` / `buildVplArgv` MUST produce same argv as `server.py` `_build_per_shot_argv` / `_build_roi_argv` / `_build_bench_argv` / `_build_vpl_argv`; schemas (names, enums, defaults, bounds) must match. `sidecar_parity_test.go::TestSidecarArgvParity` drives both sides, compares. Argv builders split out of handlers so test runs without sidecar binary on disk — do not fold back into handlers. Float arguments formatted with `strconv.FormatFloat(v, 'f', -1, 64)` (Go) and `server.py::_fmt_float` (Python); Python `repr` alone NOT equivalent (keeps trailing `.0` on integral values, switches to exponent notation for small magnitudes; changes argv bytes). New float parameter must go through `_fmt_float`.

17. **gRPC bridge deliberately Go-only** (ADR-1184). 5 control-plane tools (`submit_job`, `get_job`, `cancel_job`, `list_jobs`, `vmaf_score_remote`) have NO Python twin: Python server ships no gRPC stack and Phase-4b architecture names Go binary controller MCP client. Legal under invariant #1: listed in `goOnlyTools`; parity test fails when one is dropped or an undeclared tool appears. Do not delete tools; do not add `grpcio` to `mcp-server/vmaf-mcp` without superseding ADR-1184. Connection targets and credentials environment-only (`VMAFX_CONTROLLER_ADDR`, `VMAFX_SERVER_ADDR`, `VMAFX_CONTROLLER_TOKEN`, `VMAFX_GRPC_TIMEOUT`) — tool argument naming host makes MCP server SSRF pivot. `impl_grpc_test.go::TestGRPCTargetsComeFromEnv` pins this; `validateRemotePath` (shape-only: absolute, no `..`, no control characters) = ONLY guard on remote path arguments (`libvmaf.ValidatePath` cannot apply to file on worker node).

18. **Sidecar binary resolution goes through `libvmaf.FindSidecarBinary`** (`pkg/libvmaf/paths.go`). Second candidate — sibling of resolved `vmaf` binary — load-bearing: single `VMAF_BIN` resolves whole family (required by vmaf-dev-mcp container after `make install`). `server.py::_sidecar_binary` mirrors order. Adding sidecar requires adding to `SidecarBinaryEnv` AND `_SIDECAR_BINARY_ENV`.

19. **Every tool reaches server through `registerTools` grouped helpers** (`tools.go`). `registerTools` list of `registerXTools(srv)` calls; tool registrations live in helpers in original order. New tool must go into one (or new helper called by `registerTools`). A `register*` function nothing calls registers nothing. Invariant #1 parity test catches dropped Python or declared Go-only tool. Schema literals marshalled by `toolRegistrar.add`, not tool literal; schema failing marshal cannot reach registered tool: `add` retains error, registers nothing, skips later registrations; `registerTools` returns error, `buildServer` fails. Never substitute default schema for failed marshal: permissive `{"type":"object"}` accepts every argument map, tool stays reachable with declared contract silently disabled, parity tests miss because they compare registered tools only. `cmd/vmafx-mcp/tool_schema_test.go` pins outcome.

20. **Auto-dispatch backend identity comes from CLI receipt, never metric counts.** `decodeVmafOutput` accepts concrete top-level `backend_used` value (`cpu`, `cuda`, `sycl`, `hip`, or `metal`) when request used `auto`; missing, generic `gpu`, `auto`, non-string, and unknown receipts become `unknown`. Metric-key counts are run observations and move as extractors change. Keep non-object JSON rejection before response annotation. Explicit subprocess requests echo requested backend; direct-cgo path remains separate, keeps `cpu (direct cgo)`.

21. **Scoring schemas + argv from generated options** (RC4 WP8, ADR-2044).
    `vmaf_score`, `vmaf_score_encoded`, `describe_worst_frames` registered via
    `toolRegistrar.addGenerated`: schema = `pkg/scoreopts` `options.gen.json`
    (generated from `core/api/vmafx.toml`, byte-identical to Python copy
    `mcp-server/vmaf-mcp/src/vmaf_mcp/options.gen.json`). Validation =
    `doc.FromMCP` (bounds, enums, reserved); flags = `cliFlag()` /
    `doc.ExtraArgs` / `scoreExtras.modelSpec`. Hand-written only: tiny_device
    vs dnn_ep fold, csv/sub/output_fmt agreement, no_reference needs
    tiny_model, backend -> `--no_<sibling>`. Do not bring back
    `scoringExtraProperties` or hand flag lists. Omitted `model` =
    `version=` + `library_defaults[VMAF_DEFAULT_MODEL_VERSION]`, never a
    literal. New scoring argument = definition entry + regenerate + both
    servers unchanged unless it needs a hand rule.
