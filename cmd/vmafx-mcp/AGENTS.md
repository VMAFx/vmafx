# AGENTS.md — cmd/vmafx-mcp

Go MCP server: exposes 24 tools (vmaf_score, list_models, ...) to MCP
clients (Claude Desktop, Cursor, in-tree gRPC server). Wraps libvmaf C library
via 2 paths: legacy `exec.Command(vmaf, ...)` subprocess path (default) and
direct cgo path from ADR-0931 (opt-in via `VMAFX_MCP_DIRECT=1`).

## Composition root (golusoris fx, ADR-1119 Phase-1 PR-5)

`main.go` = `fx.New(...).Run()` over `internal/app/bootstrap.Base`
(golusoris.Core = config + slog + clock + id + validate + crypto, plus
`otel.Module`, build-version supply). Mirrors sibling migrations
(`cmd/vmafx-server`, `cmd/vmafx-node`). Shape:

```go
fx.New(
    bootstrap.Base,
    fx.Replace(config.Options{EnvPrefix: "VMAFX_", Delimiter: ".", Watch: true}),
    bootstrap.FxLogger(),
    fx.Provide(buildMCPServer),  // (*slog.Logger, *config.Config) -> (*mcp.Server, error); reuses buildServer
    fx.Invoke(runMCPTransport),  // wires the transport in a lifecycle hook
).Run()
```

Key facts:

- **The MCP server is NOT a golusoris server module.** golusoris ships no MCP
  module -> no `golusoris.HTTP` / `grpc.Module` in graph. Transport (stdio or
  streamable-HTTP from config) owned in `runMCPTransport` fx lifecycle hook:
  `OnStart` launches on goroutine, `OnStop` drains. If golusoris adds MCP module
  later, fold hook into module. Until then, do not express transport as
  framework module.
- **`buildServer` is the domain seam.** `buildMCPServer` = thin fx provider
  calling `buildServer(*slog.Logger) (*mcp.Server, error)` (server.go).
  `*config.Config` param exists for fx signature / config-driven wiring;
  `buildServer` itself needs only logger. Tests call `buildServer(nil)`
  directly and must check the error. The error return is load-bearing, not
  decoration: it is the only path by which a tool whose input schema failed to
  marshal stops the process instead of being served with no argument
  validation (invariant #19). Do not widen the signature further, and do not
  reduce it back to a single return value — an fx provider that cannot fail
  has nowhere to put that failure but a log line.
- **Config keys (env prefix `VMAFX_`, `.` delimiter — every `_` becomes `.`):**
  `VMAFX_MCP_TRANSPORT` -> `mcp.transport` (default `stdio`);
  `VMAFX_MCP_HTTP_ADDR` -> `mcp.http.addr` (default `:3000`, full listen
  address). `VMAF_BIN` and `VMAFX_MCP_DIRECT` read directly by tool handlers
  via `os.Getenv`, NOT through koanf — contract unchanged.
- **Interim env bridge.** `main()` bridges `VMAFX_LOG_LEVEL → LOG_LEVEL` and
  `VMAFX_LOG_FORMAT → LOG_FORMAT` before `fx.New` (golusoris#234, v0.4.0 log
  module reads bare `LOG_LEVEL`). Delete in lockstep with sibling binaries once
  carrying golusoris tag lands.

## Rebase-sensitive invariants

1. **Tool name + schema parity with Python** (`tools.go`): every tool name and
   required-field set MUST match Python `mcp-server/vmaf-mcp/` server
   `_list_tools()` output. Adding Go-only tool or renaming required field breaks
   IDE MCP clients configured against Python schema.
   `server_test.go::TestToolListMatchesPython` and `TestToolSchemasMatchPython`
   enforce this.

2. **Direct/subprocess dispatcher** (`impl.go`, `impl_direct.go`): every tool
   handler with both paths MUST dispatch via
   `if directPathEnabled() { runFooDirect(...) } else { runFoo(...) }`.
   Direct variant responsible for fallback to subprocess variant on unhandled
   cases (GPU backends, .onnx models, unresolvable model versions in Phase 1).
   Keeps env gate safe to leave on globally. See ADR-0931 §"Fallback flag" and
   `docs/architecture/mcp-cgo-direct-migration.md`.

3. **`VMAFX_MCP_DIRECT` is strict** (`impl_direct.go::directPathEnabled`): only
   exact string `"1"` enables direct path. `"true"`, `"yes"`, `"on"` all
   treated as off. Phase 3 may relax this; until then strict check asserted by
   `impl_direct_test.go::TestDirectPathEnabled`.

4. **Marker stream** (`pkg/libvmaf.LogDirectPathSelected`):
   "VMAFX_MCP_DIRECT=1 ... direct cgo scoring path" marker writes to stderr
   exactly once per process. Operators rely on marker to confirm path took
   effect. Routing through slog or stdout breaks convention.

5. **Response shape additions are additive only**: direct path adds
   `backend_used = "cpu (direct cgo)"` and `frame_count` to existing subprocess
   JSON shape. Phase 2/3 will populate `frames` and per-feature pooled scores.
   Existing keys (`pooled_metrics.vmaf.mean`, `backend_requested`,
   `mismatched_model_warning`) MUST remain identical to subprocess output.

6. **Model arg compatibility** (`impl_direct.go::resolveModelArgToPath`):
   accepts 4 MCP-level model forms (`version=NAME`, `path=ABS`, bare stem,
   abs/rel path). New forms require coordinated update to Python server
   resolver (`mcp-server/vmaf-mcp/src/vmaf_mcp/`).

7. **gosec G304 / G204 contract** (every `os.ReadFile` / `os.Open` /
   `exec.Command*` in `impl.go`): path or command variable consumed by these
   calls MUST either (a) round-trip through `libvmaf.ValidatePath`
   (caller-supplied paths), (b) originate from `os.CreateTemp` / `os.MkdirTemp`
   (locally-generated temp paths), or (c) come from `libvmaf.FindBinary` /
   `findVmafTune` / `exec.LookPath` of fixed binary name. Subprocess call site
   or file read without gate = directly attacker-influenced path; add
   validation or annotate `// #nosec G204` / `// #nosec G304` with citation
   naming protecting helper. CI gate (`gosec -exclude-generated` in
   `go-ci.yml`) blocks merge until one of two is true.
   `cmd/vmafx-mcp/impl_gosec_test.go::TestDescribeModelRejectsTraversal` pins
   `describeModel` allowlist; equivalent regressions for new tools belong next
   to it. See [ADR-0983](../../docs/adr/0983-gosec-findings-fix-sweep.md).

8. **Handler test coverage** (`impl_handlers_test.go`): covers "binary not
   found" fast-fail branches of every tool handler, `findVmafTune` env-override,
   `parseArgs` nil/valid/invalid, ambiguous model-stem path in `describeModel`,
   and real filesystem walks for `handleListModels` and `handleListExtractors`.
   When adding new tool handler: add >=1 error-path test not requiring external
   binary (use `t.Setenv("VMAF_BIN", "/nonexistent/...")` or similar). Statement
   coverage target: 50 %+ on `cmd/vmafx-mcp` package.

9. **Context propagation invariant** (ADR-1085): every subprocess launched from
   tool handler MUST use `exec.CommandContext(ctx, ...)` where `ctx` comes from
   MCP dispatch layer. `exec.Command(...)` (without context) is forbidden for
   tool-handler subprocesses — bare `exec.Command` leaves orphan processes
   running when MCP client disconnects. Only exception = `probeBackends` (uses
   fresh `context.WithTimeout` because module-level background cache fill, not
   per-request call). `runVmafScore`, `delegateToPythonEval`, and
   `runVmafScoreDirect` all carry `ctx context.Context` parameter by design; new
   subprocess functions must follow same pattern.

10. **Go↔Python byte-identical scoring surface** (ADR-1117 / #1240): Go server
    (`tools.go` `scoringExtraProperties()` + `impl.go` `parseScoreExtras` /
    `buildVmafArgv`) and Python server (`server.py`
    `_scoring_extra_properties()` + `_extras_from_args` / `_build_vmaf_argv`)
    MUST declare same `vmaf_score` / `vmaf_score_encoded` input schema
    (property names, types, enums, defaults, required-ness, including device
    selectors `--cpumask`, `--gpumask`, `--sycl_device`, `--hip_device`,
    `--metal_device`, `output_fmt`, `subsample`, tiny-AI flags) AND build same
    `vmaf` CLI argv for given input. Client must get same result from either
    server — with 1 documented exception in invariant #15. When adding or
    changing scoring param, change BOTH sides and keep canonical argv ORDER
    identical (e.g. `--subsample` only when `>1`, emitted before extras).
    `TestGoAndPythonArgvParity` (Go) and `test_parity_argv.py` (Python) enforce
    byte-identical CLI invocation; `score_extras_test.go` and
    `tests/test_score_extras_adr1117.py` pin schema and bounds validation
    against `core/tools/cli_parse.c`. Allowed roots must include
    `python/test/resource/yuv` in both servers so worktree symlinks resolve.

11. **stdio-stdout purity** (ADR-1119, `main.go`): in stdio mode,
    `mcp.StdioTransport` owns `os.Stdin` / `os.Stdout` for JSON-RPC framing.
    Single stray write to stdout corrupts stream and breaks every IDE MCP
    client. fx composition root keeps stdout clean: golusoris `log` writes to
    STDERR, `otel.Module` is OTLP-gRPC (no stdout writes; silent no-op when no
    exporter configured), and `bootstrap.FxLogger()` routes fx lifecycle
    events through slog -> STDERR (NOT fx default printer). Anything added to
    graph touching stdout — `fx.WithLogger` printing to stdout, stdout OTel
    exporter, bare `fmt.Println` / `os.Stdout.Write` in composition root or
    provider — MUST be gated off (or routed to stderr) in stdio mode. HTTP
    transport lacks this constraint (serves over TCP), but same providers run
    in both modes; safe default: never write to stdout from anywhere in package
    except MCP transport. Verify after composition-root change by driving
    stdio binary with JSON-RPC `initialize` + `tools/list`; assert stdout
    carries only valid JSON-RPC objects (logs on stderr).

12. **probe_backend parity invariant** (ADR-0608 follow-up): Go
    `handleProbeBackend` (`impl.go`) and Python `_probe_backend` (`server.py`)
    MUST use same synthetic probe frame and same `runtime_healthy` predicate.
    Frame = **64x64** 4:2:0 8-bit mid-grey (`probeYUVWidth`/`probeYUVHeight` ↔
    `_PROBE_YUV_WIDTH`/`_PROBE_YUV_HEIGHT`): must NOT shrink below 36px per
    dimension (CUDA ADM kernel silently returns null score under minimum; naive
    `runtime_healthy=true` misreports healthy backend). `runtime_healthy` =
    true iff subprocess exits 0 AND pooled `vmaf.mean` score non-null (Go
    additionally rejects non-finite floats); on null/non-finite score both
    servers set `runtime_healthy=false` with error string
    `"vmaf returned exit 0 but score was null"`. `TestProbeYUVDimensions` /
    `TestScoreIsHealthy` (Go) and `tests/test_probe_backend_pr850.py` (Python)
    pin both sides.

13. **HTTP transport security parity** (ADR-0967): when `mcp.transport=http`,
    Go transport (`http_security.go::securityMiddleware` + `applyBindHost`,
    wired in `main.go::runMCPTransport`) and Python transport
    (`http_transport.py::_make_security_middleware` + `_resolve_bind_host`)
    MUST enforce same hardening under same env contract:
    `VMAFX_MCP_HTTP_TOKEN` (bearer token, constant-time compare —
    `crypto/subtle.ConstantTimeCompare` ↔ `hmac.compare_digest`),
    `VMAFX_MCP_HTTP_NO_AUTH=1` (explicit opt-out), **refuse-all 401 when neither
    is set**, **4 MiB** request-body limit (`http.MaxBytesReader` +
    Content-Length pre-flight -> 413 ↔ `MAX_REQUEST_BODY_BYTES`), loopback-only
    default bind (`VMAFX_MCP_HTTP_BIND`, default `127.0.0.1`). Client must get
    same accept/reject decision from either server. `TestSecurityMiddleware*` /
    `TestApplyBindHost` (Go) and `tests/test_http_transport.py` security block
    (Python) pin both sides. Do not relax refuse-all default or widen bind
    default without changing BOTH servers and ADR-0967.

14. **Score-precision default parity** (ADR-0119 / ADR-1117): every MCP scoring
    path — Go stdio/subprocess (`impl.go`), Go direct-cgo fallback
    (`impl_direct.go`), Python stdio (`server.py`), Python HTTP `/v1/score`
    (`http_transport.py`) — MUST default `precision` arg to `legacy` (`%.6f`,
    documented C-CLI default). Do not reintroduce `"17"` default on any single
    path: client must get same numeric format regardless of server / transport /
    dispatch path serving request.

15. **One `vmafx.mcp.tool` span per tool call, from `addRawTool`** (`tools.go`,
    ADR-0782 / ADR-1119): registration wrapper = binary's only per-request span
    site on both transports. Tags `AttrMCPTool` with tool name; records parse /
    handler / marshal failures on span status while response keeps invariant
    #2's `IsError` contract (never non-nil error to SDK). HTTP transport handler
    chain: `bootstrap.TraceHTTPHandler(securityMiddleware(mcp handler))` —
    tracing outermost so 401/413 rejections traced (`main.go`). OTel init from
    `bootstrap.Base` (no-op without endpoint; stdio purity #11 unaffected
    because OTel never writes to stdout).
    `otel_test.go::TestToolCallEmitsSpan` and `TestOTelWiredThroughBootstrap`
    lock this.

16. **Sidecar parity is required** (ADR-1184, #1240). 15 classic tools and
    4 sidecar tools (`vmaf_per_shot`, `vmaf_roi`, `vmaf_bench`, `vmaf_vpl`)
    have byte-compatible Python twins. `impl_sidecar.go` `buildPerShotArgv` /
    `buildRoiArgv` / `buildBenchArgv` / `buildVplArgv` MUST produce same argv as
    `server.py` `_build_per_shot_argv` / `_build_roi_argv` /
    `_build_bench_argv` / `_build_vpl_argv`; schemas (names, enums, defaults,
    bounds) must match. `sidecar_parity_test.go::TestSidecarArgvParity` drives
    both sides, compares. Argv builders split out of handlers so test runs
    without sidecar binary on disk — do not fold back into handlers. Float
    arguments formatted with `strconv.FormatFloat(v, 'f', -1, 64)` (Go) and
    `server.py::_fmt_float` (Python); Python `repr` alone NOT equivalent
    (keeps trailing `.0` on integral values, switches to exponent notation for
    small magnitudes; changes argv bytes). New float parameter must go through
    `_fmt_float`.

17. **The gRPC bridge is deliberately Go-only** (ADR-1184). 5 control-plane
    tools (`submit_job`, `get_job`, `cancel_job`, `list_jobs`,
    `vmaf_score_remote`) have NO Python twin: Python server ships no gRPC stack
    and Phase-4b architecture names Go binary as controller MCP client. Legal
    under invariant #1 because parity test asserts Go is superset of Python.
    Do not "restore parity" by deleting these tools; do not add `grpcio` to
    `mcp-server/vmaf-mcp` without superseding ADR-1184. Connection targets and
    credentials environment-only (`VMAFX_CONTROLLER_ADDR`, `VMAFX_SERVER_ADDR`,
    `VMAFX_CONTROLLER_TOKEN`, `VMAFX_GRPC_TIMEOUT`) — tool argument naming host
    makes MCP server SSRF pivot. `impl_grpc_test.go::TestGRPCTargetsComeFromEnv`
    pins this; `validateRemotePath` (shape-only: absolute, no `..`, no control
    characters) = ONLY guard on remote path arguments (`libvmaf.ValidatePath`
    cannot apply to file on worker node).

18. **Sidecar binary resolution goes through `libvmaf.FindSidecarBinary`**
    (`pkg/libvmaf/paths.go`). Second candidate — sibling of resolved `vmaf`
    binary — load-bearing: single `VMAF_BIN` resolves whole family (required
    by vmaf-dev-mcp container after `make install`).
    `server.py::_sidecar_binary` mirrors same order. Adding sidecar requires
    adding to `SidecarBinaryEnv` AND to `_SIDECAR_BINARY_ENV`.

19. **Every tool reaches the server through one of `registerTools`' grouped
    helpers** (`tools.go`). `registerTools` is a list of `registerXTools(srv)`
    calls; the tool registrations live in those helpers, in the order they were
    originally written. A new tool must go into one of them (or into a new
    helper that `registerTools` calls) — a `register*` function nothing calls
    registers nothing, and invariant #1's parity test only asserts every Python
    tool is present, so a Go-only tool silently dropped this way is not caught.
    Schema literals are marshalled by `toolRegistrar.add`, not by the tool
    literal, so a schema that fails to marshal cannot reach a registered tool:
    `add` retains the error, registers nothing, and skips every later
    registration; `registerTools` returns it and `buildServer` fails. Never
    substitute a default schema for one that failed to marshal — the permissive
    `{"type":"object"}` accepts every argument map, so the tool would stay
    reachable with its declared contract silently switched off, and the parity
    tests would not notice because they only compare the tools that *are*
    registered. `cmd/vmafx-mcp/tool_schema_test.go` pins that outcome.

20. **Auto-dispatch backend identity comes from the CLI receipt, never metric
    counts.** `decodeVmafOutput` accepts only a concrete top-level
    `backend_used` value (`cpu`, `cuda`, `sycl`, `hip`, or `metal`) when the
    request used `auto`; missing, generic `gpu`, `auto`, non-string, and unknown
    receipts become `unknown`. Metric-key counts are run observations and move
    as extractors change. Keep non-object JSON rejection before response
    annotation. Explicit subprocess requests still echo the requested backend;
    the direct-cgo path remains separate and keeps `cpu (direct cgo)`.
