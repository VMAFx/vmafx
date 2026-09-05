<!-- markdownlint-disable MD013 MD024 -->
# AGENTS.md — mcp-server/

Orientation for agents working on the MCP (Model Context Protocol) server.
Parent: [../AGENTS.md](../AGENTS.md).

## Scope

A Python JSON-RPC server that exposes libvmaf capabilities as MCP tools
for editor/agent consumers.

```text
mcp-server/
  vmaf-mcp/
    pyproject.toml
    src/                    # tool implementations + JSON-RPC glue
    tests/
```

## Exposed tools

Locked in [ADR-0009](../docs/adr/0009-mcp-server-tool-surface.md):

- `vmaf_score` — score a ref/dist pair, returning per-frame + aggregate
- `list_models` — enumerate registered VMAF models (`model/`) + tiny models (`model/tiny/`)
- `list_backends` — SIMD caps + GPU devices present on the host
- `run_benchmark` — run the full multi-fixture benchmark harness (`bench_all.sh`) with no
  arguments; per-pair scoring belongs in `vmaf_score` (ADR-0513)
- `eval_model_on_split` — evaluate a tiny-AI ONNX regressor on a parquet split
- `compare_models` — rank ONNX regressors on the same split
- `describe_worst_frames` — local VLM describes the N frames with lowest VMAF score

Later additions (the ADR-0608 P1 wave, then #1240):

- `probe_backend`, `vmaf_version`, `vmaf_score_encoded`, `list_extractors`,
  `describe_model`, `run_compare`, `run_ladder`, `run_tune_per_shot`
- `vmaf_per_shot`, `vmaf_roi`, `vmaf_bench`, `vmaf_vpl` — one per sidecar CLI
  binary built next to `vmaf` in [`../core/tools/`](../core/tools/), documented in
  [`../docs/mcp/tools.md`](../docs/mcp/tools.md)

## Ground rules

- **Parent rules** apply (see [../AGENTS.md](../AGENTS.md)).
- **Never shell out to `vmaf` with user-controlled args** — the MCP server
  is the trusted front-end; tool arguments are untrusted. Use the Python
  bindings in [../compat/python-vmaf/](../compat/python-vmaf/) or in-process libvmaf via
  ctypes / cffi. If shelling out is unavoidable, pass args as a list and
  validate against an explicit schema.
- **No paths escape the caller's workspace** — any filesystem arg is
  resolved via `realpath` and rejected if it escapes a configured root.
- **Tiny-AI surface rule applies**: MCP tools that touch the tiny-AI path
  (e.g. `describe_worst_frames`) ship docs under `docs/ai/` in the same PR.
  See [ADR-0042](../docs/adr/0042-tinyai-docs-required-per-pr.md).

## Rebase-sensitive invariants

**The sidecar tools' argv must stay byte-identical to the Go server's**
(ADR-1184, #1240). `_build_per_shot_argv`, `_build_roi_argv`,
`_build_bench_argv` and `_build_vpl_argv` in
[`src/vmaf_mcp/server.py`](vmaf-mcp/src/vmaf_mcp/server.py) are twins of
`buildPerShotArgv` / `buildRoiArgv` / `buildBenchArgv` / `buildVplArgv` in
[`../cmd/vmafx-mcp/impl_sidecar.go`](../cmd/vmafx-mcp/impl_sidecar.go);
`cmd/vmafx-mcp/sidecar_parity_test.go` runs both and compares. They are separate
from the handlers precisely so that test can call them without a sidecar binary
on disk — do not inline them back.

**Float arguments go through `_fmt_float`, never `repr` or f-string
formatting.** Go writes `strconv.FormatFloat(v, 'f', -1, 64)`: shortest
round-trip, never exponent notation, no trailing `.0`. Python's `repr(90.0)` is
`"90.0"` and `repr(1e-05)` is `"1e-05"`; both differ from Go's bytes and would
break the argv-parity gate.

**The five gRPC control-plane tools are Go-only and must NOT be added here**
(`submit_job`, `get_job`, `cancel_job`, `list_jobs`, `vmaf_score_remote`).
[ADR-1184](../docs/adr/1184-mcp-grpc-bridge-go-only.md) records the decision: this
server deliberately has no gRPC stack, because ADR-0704's whole motivation for the
Go port was removing the Python wheel chain from the deployment path. Adding
`grpcio` and vendored Python stubs here requires superseding that ADR.
`tests/test_smoke_e2e.py::test_list_tools_returns_expected_names` pins the exact
Python tool set, so an accidental addition fails the suite.

**`run_benchmark` takes no positional arguments** (ADR-0517). `bench_all.sh` is a
fixed-fixture suite. Do not add `ref`/`dis`/`width`/`height` args back — they
corrupt `$@` inside the sourced Intel oneAPI `setvars.sh` and cause a silent abort.

**`bench_all.sh` must have `set +u` / `set -u` around the `source setvars.sh` call**
(ADR-0517). `setvars.sh` references variables (`SETVARS_ARGS`, `ia32`) that may be
unset in the calling context; `set -u` aborts on those references and bypasses `|| true`.

## Governing ADRs

- [ADR-0005](../docs/adr/0005-framework-adaptation-full-scope.md) — framework scope includes MCP.
- [ADR-0009](../docs/adr/0009-mcp-server-tool-surface.md) — the four initial tools.
- [ADR-0036](../docs/adr/0036-tinyai-wave1-scope-expansion.md) — `describe_worst_frames` on the Wave 1 list.
- [ADR-0042](../docs/adr/0042-tinyai-docs-required-per-pr.md) — doc rule.

## Rebase-sensitive invariants

- **`_probe_backends` reads `vmaf --help`, not `--version` (ADR-0509,
  Bug A).** Every backend the local `vmaf` binary was compiled with
  surfaces in `--help` as a `--no_<backend>` disable flag. The
  `--version` banner does NOT list compiled-in GPU backends on this
  fork, so the historical banner-grep mis-reported CUDA as
  unavailable on hosts where the kernel + driver + binary all
  supported it (e.g. the `vmaf-dev-mcp` container). Results are
  cached per-binary-path for the server-process lifetime so
  `vmaf_score` does not fork a subprocess per call. A regression
  that switches the probe source back to `--version` re-introduces
  the silent "CUDA-not-available" false-negative class.
- **Default allowlist includes `/workspace/python/test/resource`
  alongside the host-relative `<repo>/python/test/resource`
  (ADR-0509, Bug B).** The container at
  [`dev/Containerfile`](../dev/Containerfile) bind-mounts the repo
  root at `/workspace/`, so the Netflix golden YUVs are at the
  absolute container path; without the absolute entry every
  container-side MCP demo had to set
  `VMAF_MCP_ALLOW=/workspace/python/test/resource` first.
  `VMAF_MCP_ALLOW` env-var override is preserved and additive (it
  extends the default list, does not replace it).
- [ADR-0517](../docs/adr/0517-mcp-run-benchmark-repair.md) — `run_benchmark` repair.
- **HTTP transport optional dep group (PR #1583, ADR-0701).** The
  `[http]` optional dependency group in `vmaf-mcp/pyproject.toml`
  (`aiohttp`, `prometheus-client`) must be preserved on any rebase or
  `pyproject.toml` edit. The `--transport http` flag in
  `src/vmaf_mcp/server.py::main()` dispatches to
  `src/vmaf_mcp/http_transport.py`. The transport dispatch block
  (`if args.transport == "http":`) must return before starting the stdio
  event loop. The production server image installs both `[eval]` and `[http]`;
  omitting the latter makes its HTTP entrypoint fail at runtime. Netflix
  upstream has no MCP server; this entire subtree is fork-local and will
  never merge upstream.
- **MCP 2.x uses constructor-registered low-level handlers (ADR-1129).**
  `Server.list_tools()` / `Server.call_tool()` decorators and
  `Server.request_context` were removed in mcp 2.1. Keep `_mcp_list_tools`
  and `_mcp_call_tool` registered through `Server(..., on_list_tools=...,
  on_call_tool=...)`; the call adapter must continue translating dispatcher
  exceptions into `CallToolResult(isError=True)` and exposing the request
  session through the task-local context only while a call is active.
- **HTTP transport requires explicit env opt-in for 0.0.0.0 bind; auth
  defaults on (ADR-0967).** The default bind host for `--transport http`
  is `127.0.0.1` (loopback-only). To listen on all interfaces (required
  for pod-network reachability in Kubernetes), set
  `VMAFX_MCP_HTTP_BIND=0.0.0.0`. Authentication is enforced by default:
  if `VMAFX_MCP_HTTP_TOKEN` is unset and `VMAFX_MCP_HTTP_NO_AUTH` is
  also unset, the server rejects every request with 401. A regression
  that reverts `_resolve_bind_host()` to return `"0.0.0.0"` or removes
  the security middleware from `_make_app()` re-introduces the Round 26
  audit finding A.1 vulnerabilities.

- **Required-argument tools rely on the shared `_call_tool`
  `KeyError`→`ValueError` wrapper** (`tool 'X' missing required argument:
  'key'`). Read required args with `arguments["key"]` and let the missing
  key raise `KeyError`; do **not** add a bespoke per-tool
  `if "key" not in arguments: raise ValueError("'key' is required ...")`
  guard. Bespoke messages diverge from the uniform string that
  `test_call_tool_missing_*_raises_value_error` asserts (regex
  `missing required argument.*'key'`) and silently red the non-required
  `MCP Smoke` lane (the `probe_backend` 2026-06-20 fix).

- **Go↔Python byte-identical scoring surface (ADR-1117 / #1240).** The Python
  server (`server.py` `_scoring_extra_properties()` + `_extras_from_args` /
  `_build_vmaf_argv`) and the Go server (`cmd/vmafx-mcp/tools.go` +
  `impl.go` `parseScoreExtras` / `buildVmafArgv`) MUST declare the same
  `vmaf_score` / `vmaf_score_encoded` input schema (property names, types,
  enums, defaults, required-ness, including device selectors `--cpumask`,
  `--gpumask`, `--sycl_device`, `--hip_device`, `--metal_device`, `output_fmt`,
  `subsample`, and tiny-AI flags) AND build the identical `vmaf` CLI argv
  for a given input. Any additions must update both servers in lockstep and
  preserve canonical flag ordering. `tests/test_parity_argv.py` and
  `TestGoAndPythonArgvParity` enforce cross-server invocation parity. Both servers
  include `python/test/resource/yuv` in allowed roots so worktree symlinks resolve.
