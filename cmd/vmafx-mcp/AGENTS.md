# AGENTS.md — cmd/vmafx-mcp

Go MCP server that exposes 24 tools (vmaf_score, list_models, ...) to MCP
clients (Claude Desktop, Cursor, the in-tree gRPC server). Wraps the libvmaf
C library via two paths: the legacy `exec.Command(vmaf, ...)` subprocess
path (default) and the direct cgo path introduced by ADR-0931 (opt-in via
`VMAFX_MCP_DIRECT=1`).

## Composition root (golusoris fx, ADR-1119 Phase-1 PR-5)

`main.go` is an `fx.New(...).Run()` over `internal/app/bootstrap.Base`
(golusoris.Core = config + slog + clock + id + validate + crypto, plus
`otel.Module` and the build-version supply). It mirrors the sibling
migrations (`cmd/vmafx-server`, `cmd/vmafx-node`). The shape is:

```go
fx.New(
    bootstrap.Base,
    fx.Replace(config.Options{EnvPrefix: "VMAFX_", Delimiter: ".", Watch: true}),
    bootstrap.FxLogger(),
    fx.Provide(buildMCPServer),  // (*slog.Logger, *config.Config) -> *mcp.Server; reuses buildServer
    fx.Invoke(runMCPTransport),  // wires the transport in a lifecycle hook
).Run()
```

Key facts a future agent must keep straight:

- **The MCP server is NOT a golusoris server module.** golusoris ships no MCP
  module, so there is no `golusoris.HTTP` / `grpc.Module` in the graph. The
  transport (stdio or streamable-HTTP, selected from config) is owned in the
  `runMCPTransport` fx lifecycle hook: `OnStart` launches it on a goroutine,
  `OnStop` drains it. If golusoris later adds an MCP module, fold the hook into
  it. Until then, do not try to express the transport as a framework module.
- **`buildServer` is the unchanged domain seam.** `buildMCPServer` is a thin fx
  provider that calls the existing `buildServer(*slog.Logger) *mcp.Server`
  (server.go). The `*config.Config` param exists for the fx signature /
  forward-looking config-driven wiring; `buildServer` itself only needs the
  logger. Tests still call `buildServer(nil)` directly — do not change its
  signature.
- **Config keys (env prefix `VMAFX_`, `.` delimiter — every `_` becomes `.`):**
  `VMAFX_MCP_TRANSPORT` → `mcp.transport` (default `stdio`);
  `VMAFX_MCP_HTTP_ADDR` → `mcp.http.addr` (default `:3000`, a full listen
  address). `VMAF_BIN` and `VMAFX_MCP_DIRECT` are read directly by the tool
  handlers via `os.Getenv`, NOT through koanf — that contract is unchanged.
- **Interim env bridge.** `main()` bridges `VMAFX_LOG_LEVEL → LOG_LEVEL` and
  `VMAFX_LOG_FORMAT → LOG_FORMAT` before `fx.New` (golusoris#234, the v0.4.0 log
  module reads bare `LOG_LEVEL`). Delete in lockstep with the sibling binaries
  once the carrying golusoris tag lands.

## Rebase-sensitive invariants

1. **Tool name + schema parity with Python** (`tools.go`): every tool name
   and required-field set MUST match the Python `mcp-server/vmaf-mcp/`
   server's `_list_tools()` output. Adding a Go-only tool or renaming a
   required field breaks IDE MCP clients that were configured against the
   Python schema. `server_test.go::TestToolListMatchesPython` and
   `TestToolSchemasMatchPython` enforce this.

2. **Direct/subprocess dispatcher** (`impl.go`, `impl_direct.go`): every
   tool handler that has both paths MUST dispatch via
   `if directPathEnabled() { runFooDirect(...) } else { runFoo(...) }`.
   The direct variant is the responsible party for falling back to the
   subprocess variant on cases it does not handle (GPU backends, .onnx
   models, unresolvable model versions in Phase 1). This keeps the env
   gate safe to leave on globally. See ADR-0931 §"Fallback flag" and
   `docs/architecture/mcp-cgo-direct-migration.md`.

3. **`VMAFX_MCP_DIRECT` is strict** (`impl_direct.go::directPathEnabled`):
   only the exact string `"1"` enables the direct path. `"true"`, `"yes"`,
   `"on"` are all treated as off. Phase 3 may relax this, but until then
   the strict check is what
   `impl_direct_test.go::TestDirectPathEnabled` asserts.

4. **Marker stream** (`pkg/libvmaf.LogDirectPathSelected`): the
   "VMAFX_MCP_DIRECT=1 ... direct cgo scoring path" marker writes to
   stderr exactly once per process. Operators rely on it to confirm the
   path took effect. Routing it through slog or stdout breaks the
   convention.

5. **Response shape additions are additive only**: the direct path adds
   `backend_used = "cpu (direct cgo)"` and `frame_count` to the existing
   subprocess JSON shape. Phase 2/3 will populate `frames` and per-feature
   pooled scores. Existing keys (`pooled_metrics.vmaf.mean`,
   `backend_requested`, `mismatched_model_warning`) MUST remain
   identical to the subprocess output.

6. **Model arg compatibility** (`impl_direct.go::resolveModelArgToPath`):
   accepts the four MCP-level model forms (`version=NAME`, `path=ABS`,
   bare stem, abs/rel path). New forms require a coordinated update to
   the Python server's resolver (`mcp-server/vmaf-mcp/src/vmaf_mcp/`).

7. **gosec G304 / G204 contract** (every `os.ReadFile` / `os.Open` /
   `exec.Command*` in `impl.go`): any path or command variable consumed
   by these calls MUST either (a) round-trip through
   `libvmaf.ValidatePath` (caller-supplied paths), (b) originate from
   `os.CreateTemp` / `os.MkdirTemp` (locally-generated temp paths), or
   (c) come from `libvmaf.FindBinary` / `findVmafTune` / an
   `exec.LookPath` of a fixed binary name. Adding a new subprocess call
   site or file read without one of these gates means the new path is
   directly attacker-influenced; either add the validation or annotate
   `// #nosec G204` / `// #nosec G304` with a citation that names the
   protecting helper. The CI gate (`gosec -exclude-generated` in
   `go-ci.yml`) blocks the merge until one of the two is true.
   `cmd/vmafx-mcp/impl_gosec_test.go::TestDescribeModelRejectsTraversal`
   pins the `describeModel` allowlist; equivalent regressions for new
   tools belong next to it. See
   [ADR-0983](../../docs/adr/0983-gosec-findings-fix-sweep.md).

8. **Handler test coverage** (`impl_handlers_test.go`): the file covers the
   "binary not found" fast-fail branches of every tool handler, the
   `findVmafTune` env-override, `parseArgs` nil/valid/invalid, the ambiguous
   model-stem path in `describeModel`, and the real filesystem walks for
   `handleListModels` and `handleListExtractors`. When adding a new tool
   handler: add at least one error-path test that does not require an external
   binary (use `t.Setenv("VMAF_BIN", "/nonexistent/...")` or similar).
   Statement coverage target: 50 %+ on the `cmd/vmafx-mcp` package.

9. **Context propagation invariant** (ADR-1085): every subprocess launched from
   a tool handler MUST use `exec.CommandContext(ctx, ...)` where `ctx` comes
   from the MCP dispatch layer. `exec.Command(...)` (without context) is
   forbidden for tool-handler subprocesses — bare `exec.Command` leaves
   orphan processes running when the MCP client disconnects. The only
   exception is `probeBackends` (which uses its own fresh `context.WithTimeout`)
   because it is a module-level background cache fill, not a per-request call.
   `runVmafScore`, `delegateToPythonEval`, and `runVmafScoreDirect` all carry
   a `ctx context.Context` parameter by design; new subprocess functions must
   follow the same pattern.

10. **Go↔Python byte-identical scoring surface** (ADR-1117 / #1240): the Go server
   (`tools.go` `scoringExtraProperties()` + `impl.go` `parseScoreExtras` /
   `buildVmafArgv`) and the Python server
   (`server.py` `_scoring_extra_properties()` + `_extras_from_args` /
   `_build_vmaf_argv`) MUST declare the same `vmaf_score` /
   `vmaf_score_encoded` input schema (property names, types, enums, defaults,
   required-ness, including device selectors `--cpumask`, `--gpumask`,
   `--sycl_device`, `--hip_device`, `--metal_device`, `output_fmt`,
   `subsample`, and tiny-AI flags) AND build the same `vmaf` CLI argv for a
   given input. A client must get the same result from either server — with the
   one documented exception in invariant #15. When you
   add or change a scoring param, change BOTH sides and keep the canonical
   argv ORDER identical (e.g. `--subsample` only when `>1`, emitted before the
   extras). `TestGoAndPythonArgvParity` (Go) and `test_parity_argv.py` (Python)
   enforce byte-identical CLI invocation; `score_extras_test.go` and
   `tests/test_score_extras_adr1117.py` pin schema and bounds validation
   against `core/tools/cli_parse.c`. Allowed roots must include
   `python/test/resource/yuv` in both servers so worktree symlinks resolve.

11. **stdio-stdout purity** (ADR-1119, `main.go`): in stdio mode the
   `mcp.StdioTransport` owns `os.Stdin` / `os.Stdout` for the JSON-RPC framing.
   A single stray write to stdout corrupts the stream and breaks every IDE MCP
   client. The fx composition root keeps stdout clean: golusoris `log` writes to
   STDERR, `otel.Module` is OTLP-gRPC (no stdout writes; a silent no-op when no
   exporter is configured), and `bootstrap.FxLogger()` routes fx's lifecycle
   events through slog → STDERR (NOT fx's default printer). Anything added to the
   graph that could touch stdout — an `fx.WithLogger` that prints to stdout, a
   stdout OTel exporter, a bare `fmt.Println` / `os.Stdout.Write` in the
   composition root or a provider — MUST be gated off (or routed to stderr) in
   stdio mode. The HTTP transport does not share this constraint (it serves over
   TCP), but the same providers run in both modes, so the safe default is: never
   write to stdout from anywhere in this package except the MCP transport.
   Verify after any composition-root change by driving the stdio binary with a
   JSON-RPC `initialize` + `tools/list` and asserting stdout carries only valid
   JSON-RPC objects (logs land on stderr).

12. **probe_backend parity invariant** (ADR-0608 follow-up): the Go
   `handleProbeBackend` (`impl.go`) and the Python `_probe_backend`
   (`server.py`) MUST use the same synthetic probe frame and the same
   `runtime_healthy` predicate. The frame is **64x64** 4:2:0 8-bit mid-grey
   (`probeYUVWidth`/`probeYUVHeight` ↔ `_PROBE_YUV_WIDTH`/`_PROBE_YUV_HEIGHT`):
   it must NOT shrink below 36px per dimension because the CUDA ADM kernel
   silently returns a null score under that minimum, which a naive
   `runtime_healthy=true` would misreport as a healthy backend. `runtime_healthy`
   is true iff the subprocess exits 0 AND the pooled `vmaf.mean` score is
   non-null (Go additionally rejects non-finite floats); on a null/non-finite
   score both servers set `runtime_healthy=false` with the error string
   `"vmaf returned exit 0 but score was null"`. `TestProbeYUVDimensions` /
   `TestScoreIsHealthy` (Go) and `tests/test_probe_backend_pr850.py` (Python)
   pin both sides.

13. **HTTP transport security parity** (ADR-0967): when `mcp.transport=http`,
   the Go transport (`http_security.go::securityMiddleware` + `applyBindHost`,
   wired in `main.go::runMCPTransport`) and the Python transport
   (`http_transport.py::_make_security_middleware` + `_resolve_bind_host`) MUST
   enforce the same hardening under the same env contract:
   `VMAFX_MCP_HTTP_TOKEN` (bearer token, constant-time compare —
   `crypto/subtle.ConstantTimeCompare` ↔ `hmac.compare_digest`),
   `VMAFX_MCP_HTTP_NO_AUTH=1` (explicit opt-out), **refuse-all 401 when neither
   is set**, a **4 MiB** request-body limit (`http.MaxBytesReader` +
   Content-Length pre-flight → 413 ↔ `MAX_REQUEST_BODY_BYTES`), and a
   loopback-only default bind (`VMAFX_MCP_HTTP_BIND`, default `127.0.0.1`). A
   client must get the same accept/reject decision from either server.
   `TestSecurityMiddleware*` / `TestApplyBindHost` (Go) and the
   `tests/test_http_transport.py` security block (Python) pin both sides. Do
   not relax the refuse-all default or widen the bind default without changing
   BOTH servers and ADR-0967.

14. **Score-precision default parity** (ADR-0119 / ADR-1117): every MCP scoring
   path — Go stdio/subprocess (`impl.go`), Go direct-cgo fallback
   (`impl_direct.go`), Python stdio (`server.py`), and Python HTTP `/v1/score`
   (`http_transport.py`) — MUST default the `precision` arg to `legacy`
   (`%.6f`, the documented C-CLI default). Do not reintroduce a `"17"` default
   on any single path: a client must get the same numeric format regardless of
   which server / transport / dispatch path served the request.

15. **Sidecar parity is required** (ADR-1184, #1240). The 15 classic tools and
   the 4 sidecar tools (`vmaf_per_shot`, `vmaf_roi`, `vmaf_bench`, `vmaf_vpl`)
   have byte-compatible Python twins. `impl_sidecar.go`'s `buildPerShotArgv` /
   `buildRoiArgv` / `buildBenchArgv` / `buildVplArgv` MUST produce the same
   argv as `server.py`'s `_build_per_shot_argv` / `_build_roi_argv` /
   `_build_bench_argv` / `_build_vpl_argv`, and the schemas (names, enums,
   defaults, bounds) must match.
   `sidecar_parity_test.go::TestSidecarArgvParity` drives both sides and
   compares. The argv builders are split out of the handlers specifically so
   that test can run without a sidecar binary on disk — do not fold them back
   into the handlers. Float arguments are formatted with
   `strconv.FormatFloat(v, 'f', -1, 64)` on the Go side and
   `server.py::_fmt_float` on the Python side; Python's `repr` alone is NOT
   equivalent, because it keeps a trailing `.0` on integral values and switches
   to exponent notation for small magnitudes, both of which change the argv
   bytes. A new float parameter must go through `_fmt_float`.

16. **The gRPC bridge is deliberately Go-only** (ADR-1184). The 5 control-plane
   tools (`submit_job`, `get_job`, `cancel_job`, `list_jobs`,
   `vmaf_score_remote`) have NO Python twin: the Python server ships no gRPC
   stack and the Phase-4b architecture names the Go binary as the controller's
   MCP client. This is legal under invariant #1 because the parity test asserts
   that Go is a superset of Python. Do not "restore parity" by deleting these
   tools, and do not add `grpcio` to `mcp-server/vmaf-mcp` without superseding
   ADR-1184. Their connection targets and credentials are environment-only
   (`VMAFX_CONTROLLER_ADDR`, `VMAFX_SERVER_ADDR`, `VMAFX_CONTROLLER_TOKEN`,
   `VMAFX_GRPC_TIMEOUT`) — a tool argument naming a host would make the MCP
   server an SSRF pivot.
   `impl_grpc_test.go::TestGRPCTargetsComeFromEnv` pins this, and
   `validateRemotePath` (shape-only: absolute, no `..`, no control characters)
   is the ONLY guard on the remote path arguments, because
   `libvmaf.ValidatePath` cannot apply to a file that lives on a worker node.

17. **Sidecar binary resolution goes through `libvmaf.FindSidecarBinary`**
   (`pkg/libvmaf/paths.go`). Its second candidate — a sibling of the resolved
   `vmaf` binary — is load-bearing: it is what makes a single `VMAF_BIN`
   resolve the whole family, which the vmaf-dev-mcp container relies on after
   `make install`. `server.py::_sidecar_binary` mirrors the same order. Adding
   a sidecar means adding it to `SidecarBinaryEnv` AND to
   `_SIDECAR_BINARY_ENV`.
