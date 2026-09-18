<!-- markdownlint-disable MD013 MD024 -->
# AGENTS.md — mcp-server/

MCP (Model Context Protocol) server orientation.
Parent: [../AGENTS.md](../AGENTS.md).

## Scope

Python JSON-RPC server; exposes libvmaf capabilities as MCP tools for
editor/agent consumers.

```text
mcp-server/
  vmaf-mcp/
    pyproject.toml
    src/                    # tool implementations + JSON-RPC glue
    tests/
```

## Exposed tools

Locked in [ADR-0009](../docs/adr/0009-mcp-server-tool-surface.md):

- `vmaf_score` — score ref/dist pair, return per-frame + aggregate
- `list_models` — enumerate registered VMAF models (`model/`) + tiny models
  (`model/tiny/`)
- `list_backends` — SIMD caps + GPU devices present on host
- `run_benchmark` — run full multi-fixture benchmark harness
  (`bench_all.sh`), no args; per-pair scoring belongs in `vmaf_score`
  (ADR-0513)
- `eval_model_on_split` — evaluate tiny-AI ONNX regressor on parquet split
- `compare_models` — rank ONNX regressors on same split
- `describe_worst_frames` — local VLM describes N frames with lowest VMAF
  score

Later additions (ADR-0608 P1 wave, then #1240):

- `probe_backend`, `vmaf_version`, `vmaf_score_encoded`, `list_extractors`,
  `describe_model`, `run_compare`, `run_ladder`, `run_tune_per_shot`
- `vmaf_per_shot`, `vmaf_roi`, `vmaf_bench`, `vmaf_vpl` — one per sidecar CLI
  binary built next to `vmaf` in [`../core/tools/`](../core/tools/),
  documented in [`../docs/mcp/tools.md`](../docs/mcp/tools.md)

## Ground rules

- **Parent rules** apply (see [../AGENTS.md](../AGENTS.md)).
- **Never shell out to `vmaf` with user-controlled args** — MCP server =
  trusted front-end; tool arguments untrusted. Use Python bindings in
  [../compat/python-vmaf/](../compat/python-vmaf/) or in-process libvmaf
  via ctypes / cffi. If shelling out unavoidable: pass args as list,
  validate against explicit schema.
- **No paths escape caller's workspace** — filesystem arg resolved via
  `realpath`, rejected if it escapes configured root.
- **Tiny-AI surface rule applies**: MCP tools touching tiny-AI path (e.g.
  `describe_worst_frames`) ship docs under `docs/ai/` in same PR.
  See [ADR-0042](../docs/adr/0042-tinyai-docs-required-per-pr.md).

## Rebase-sensitive invariants

**Sidecar tools' argv must stay byte-identical to Go server's**
(ADR-1184, #1240). `_build_per_shot_argv`, `_build_roi_argv`,
`_build_bench_argv` and `_build_vpl_argv` in
[`src/vmaf_mcp/server.py`](vmaf-mcp/src/vmaf_mcp/server.py) = twins of
`buildPerShotArgv` / `buildRoiArgv` / `buildBenchArgv` / `buildVplArgv` in
[`../cmd/vmafx-mcp/impl_sidecar.go`](../cmd/vmafx-mcp/impl_sidecar.go);
`cmd/vmafx-mcp/sidecar_parity_test.go` runs both, compares. Separate from
handlers precisely so test calls them without sidecar binary on disk —
never inline back.

**Float arguments go through `_fmt_float`, never `repr` or f-string
formatting.** Go writes `strconv.FormatFloat(v, 'f', -1, 64)`: shortest
round-trip, never exponent notation, no trailing `.0`. Python
`repr(90.0)` = `"90.0"`, `repr(1e-05)` = `"1e-05"`; both differ from Go's
bytes — either breaks argv-parity gate.

**Five gRPC control-plane tools = Go-only, must NOT be added here**
(`submit_job`, `get_job`, `cancel_job`, `list_jobs`, `vmaf_score_remote`).
[ADR-1184](../docs/adr/1184-mcp-grpc-bridge-go-only.md) records decision:
server deliberately has no gRPC stack — ADR-0704's whole motivation for Go
port was removing Python wheel chain from deployment path. Adding
`grpcio` + vendored Python stubs here requires superseding that ADR.
`tests/test_smoke_e2e.py::test_list_tools_returns_expected_names` pins
exact Python tool set; accidental addition fails suite.

**`run_benchmark` takes no positional arguments** (ADR-0517).
`bench_all.sh` = fixed-fixture suite. Never add
`ref`/`dis`/`width`/`height` args back — corrupts `$@` inside sourced
Intel oneAPI `setvars.sh`, causes silent abort.

**`bench_all.sh` must have `set +u` / `set -u` around `source setvars.sh`
call** (ADR-0517). `setvars.sh` references variables (`SETVARS_ARGS`,
`ia32`) maybe unset in calling context; `set -u` aborts on those
references, bypasses `|| true`.

## Governing ADRs

- [ADR-0005](../docs/adr/0005-framework-adaptation-full-scope.md) —
  framework scope includes MCP.
- [ADR-0009](../docs/adr/0009-mcp-server-tool-surface.md) — four initial
  tools.
- [ADR-0036](../docs/adr/0036-tinyai-wave1-scope-expansion.md) —
  `describe_worst_frames` on Wave 1 list.
- [ADR-0042](../docs/adr/0042-tinyai-docs-required-per-pr.md) — doc rule.

## Rebase-sensitive invariants

- **`_probe_backends` reads `vmaf --help`, not `--version`** (ADR-0509,
  Bug A). Every backend local `vmaf` binary compiled with surfaces in
  `--help` as `--no_<backend>` disable flag. `--version` banner does NOT
  list compiled-in GPU backends on this fork — historical banner-grep
  mis-reported CUDA unavailable on hosts where kernel + driver + binary
  all supported it (e.g. `vmaf-dev-mcp` container). Results cached
  per-binary-path for server-process lifetime, so `vmaf_score` does not
  fork subprocess per call. Regression switching probe source back to
  `--version` re-introduces silent "CUDA-not-available" false-negative
  class.
- **Default allowlist includes `/workspace/python/test/resource`
  alongside host-relative `<repo>/python/test/resource`** (ADR-0509,
  Bug B). Container at [`dev/Containerfile`](../dev/Containerfile)
  bind-mounts repo root at `/workspace/`, so Netflix golden YUVs sit at
  absolute container path; without absolute entry, every container-side
  MCP demo had to set `VMAF_MCP_ALLOW=/workspace/python/test/resource`
  first. `VMAF_MCP_ALLOW` env-var override preserved, additive — extends
  default list, does not replace it.
- [ADR-0517](../docs/adr/0517-mcp-run-benchmark-repair.md) —
  `run_benchmark` repair.
- **HTTP transport optional dep group** (PR #1583, ADR-0701). `[http]`
  optional dependency group in `vmaf-mcp/pyproject.toml` (`aiohttp`,
  `prometheus-client`) must be preserved on any rebase or
  `pyproject.toml` edit. `--transport http` flag in
  `src/vmaf_mcp/server.py::main()` dispatches to
  `src/vmaf_mcp/http_transport.py`. Transport dispatch block
  (`if args.transport == "http":`) must return before starting stdio
  event loop. Production server image installs both `[eval]` and
  `[http]`; omitting latter makes HTTP entrypoint fail at runtime.
  Netflix upstream has no MCP server; entire subtree fork-local, never
  merges upstream.
- **MCP 2.x uses constructor-registered low-level handlers** (ADR-1129).
  `Server.list_tools()` / `Server.call_tool()` decorators and
  `Server.request_context` removed in mcp 2.1. Keep `_mcp_list_tools` and
  `_mcp_call_tool` registered through `Server(..., on_list_tools=...,
  on_call_tool=...)`; call adapter must keep translating dispatcher
  exceptions into `CallToolResult(isError=True)`, exposing request
  session through task-local context only while call active.
- **HTTP transport requires explicit env opt-in for 0.0.0.0 bind; auth
  defaults on** (ADR-0967). Default bind host for `--transport http` =
  `127.0.0.1` (loopback-only). To listen on all interfaces (required for
  pod-network reachability in Kubernetes): set
  `VMAFX_MCP_HTTP_BIND=0.0.0.0`. Authentication enforced by default: if
  `VMAFX_MCP_HTTP_TOKEN` unset and `VMAFX_MCP_HTTP_NO_AUTH` also unset,
  server rejects every request with 401. Regression reverting
  `_resolve_bind_host()` to return `"0.0.0.0"`, or removing security
  middleware from `_make_app()`, re-introduces Round 26 audit finding
  A.1 vulnerabilities.

- **Required-argument tools rely on shared `_call_tool`
  `KeyError`→`ValueError` wrapper** (`tool 'X' missing required argument:
  'key'`). Read required args with `arguments["key"]`, let missing key
  raise `KeyError`; never add bespoke per-tool
  `if "key" not in arguments: raise ValueError("'key' is required ...")`
  guard. Bespoke messages diverge from uniform string
  `test_call_tool_missing_*_raises_value_error` asserts (regex
  `missing required argument.*'key'`), silently reds non-required
  `MCP Smoke` lane (`probe_backend` 2026-06-20 fix).

- **Go↔Python byte-identical scoring surface** (ADR-1117 / #1240).
  Python server (`server.py` `_scoring_extra_properties()` +
  `_extras_from_args` / `_build_vmaf_argv`) and Go server
  (`cmd/vmafx-mcp/tools.go` + `impl.go` `parseScoreExtras` /
  `buildVmafArgv`) MUST declare same `vmaf_score` / `vmaf_score_encoded`
  input schema (property names, types, enums, defaults, required-ness,
  incl. device selectors `--cpumask`, `--gpumask`, `--sycl_device`,
  `--hip_device`, `--metal_device`, `output_fmt`, `subsample`, tiny-AI
  flags). MUST also build identical `vmaf` CLI argv for given input. Any
  additions must update both servers in lockstep, preserve canonical
  flag ordering. `tests/test_parity_argv.py` and
  `TestGoAndPythonArgvParity` enforce cross-server invocation parity.
  Both servers include `python/test/resource/yuv` in allowed roots so
  worktree symlinks resolve.
