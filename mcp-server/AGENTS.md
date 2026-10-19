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
  (`bench_all.sh`), no args; per-pair scoring in `vmaf_score` (ADR-0513)
- `eval_model_on_split` — evaluate tiny-AI ONNX regressor on parquet split
- `compare_models` — rank ONNX regressors on same split
- `describe_worst_frames` — local VLM describes N frames with lowest VMAF
  score

Additions (ADR-0608 P1 wave, #1240):

- `probe_backend`, `vmaf_version`, `vmaf_score_encoded`, `list_extractors`,
  `describe_model`, `run_compare`, `run_ladder`, `run_tune_per_shot`
- `vmaf_per_shot`, `vmaf_roi`, `vmaf_bench`, `vmaf_vpl` — sidecar CLI
  binaries built next to `vmaf` in [`../core/tools/`](../core/tools/),
  documented in [`../docs/mcp/tools.md`](../docs/mcp/tools.md)

## Ground rules

- **Parent rules** apply: see [../AGENTS.md](../AGENTS.md).
- **Never shell out to `vmaf` with user-controlled args** — MCP server =
  trusted front-end; arguments untrusted. Use Python bindings in
  [../compat/python-vmaf/](../compat/python-vmaf/) or in-process libvmaf
  via ctypes / cffi. If shelling out: pass args as list, validate against schema.
- **No paths escape caller workspace** — args resolved via `realpath`,
  rejected if escaping configured root.
- **Tiny-AI surface rule applies**: tools touching tiny-AI path
  (`describe_worst_frames`) ship docs under `docs/ai/` in same PR.
  See [ADR-0042](../docs/adr/0042-tinyai-docs-required-per-pr.md).

## Rebase-sensitive invariants

**Sidecar tools' argv must stay byte-identical to Go server's**
(ADR-1184, #1240). `_build_per_shot_argv`, `_build_roi_argv`,
`_build_bench_argv` and `_build_vpl_argv` in
[`src/vmaf_mcp/server.py`](vmaf-mcp/src/vmaf_mcp/server.py) = twins of
`buildPerShotArgv` / `buildRoiArgv` / `buildBenchArgv` / `buildVplArgv` in
[`../cmd/vmafx-mcp/impl_sidecar.go`](../cmd/vmafx-mcp/impl_sidecar.go);
`cmd/vmafx-mcp/sidecar_parity_test.go` compares both without binaries on disk;
never inline back.

**Float arguments go through `_fmt_float`, never `repr` or f-string
formatting.** Go writes `strconv.FormatFloat(v, 'f', -1, 64)`: shortest
round-trip, no exponent, no `.0`. Python `repr(90.0)` = `"90.0"`,
`repr(1e-05)` = `"1e-05"`; differs from Go bytes, breaks argv-parity gate.

**Five gRPC control-plane tools = Go-only, must NOT be added here**
(`submit_job`, `get_job`, `cancel_job`, `list_jobs`, `vmaf_score_remote`).
[ADR-1184](../docs/adr/1184-mcp-grpc-bridge-go-only.md): server has no gRPC
stack — ADR-0704 Go port removed Python wheel chain from deployment path.
Adding `grpcio` + vendored Python stubs requires superseding ADR.
`tests/test_smoke_e2e.py::test_list_tools_returns_expected_names` pins
exact Python tool set.

**`run_benchmark` takes no positional arguments** (ADR-0517).
`bench_all.sh` = fixed-fixture suite. Never add `ref`/`dis`/`width`/`height`
args back: corrupts `$@` in sourced Intel oneAPI `setvars.sh`, causing abort.

**`bench_all.sh` must have `set +u` / `set -u` around `source setvars.sh`
call** (ADR-0517). `setvars.sh` references variables (`SETVARS_ARGS`,
`ia32`) maybe unset; `set -u` aborts on references, bypassing `|| true`.

## Governing ADRs

- [ADR-0005](../docs/adr/0005-framework-adaptation-full-scope.md) — MCP scope.
- [ADR-0009](../docs/adr/0009-mcp-server-tool-surface.md) — initial tools.
- [ADR-0036](../docs/adr/0036-tinyai-wave1-scope-expansion.md) — `describe_worst_frames`.
- [ADR-0042](../docs/adr/0042-tinyai-docs-required-per-pr.md) — doc rule.

## Rebase-sensitive invariants

- **`describe_worst_frames` VLM = ONNX Runtime GenAI from a local directory
  only** (ADR-1886). [`src/vmaf_mcp/vlm.py`](vmaf-mcp/src/vmaf_mcp/vlm.py):
  `VMAF_MCP_VLM_MODEL` names directory with `genai_config.json`; no download,
  no `trust_remote_code`, no torch / transformers in any extra
  (`scripts/ci/check-torch-scope.py` fails). Unset / missing -> metadata +
  note naming cause; configured model that fails to load -> raise, never
  silent metadata. Per-family image tag table = `IMAGE_TAGS` (from
  onnxruntime-genai `examples/python/common.py`).
- **`_probe_backends` reads `vmaf --list-backends`, not `--help` or `--version`**
  (ADR-1874; ADR-0509 Bug A before it). The report says per backend whether it was
  compiled in and whether its state initialises on this host; `usable` is the
  answer. `--help` names every `--no_<backend>` on every build and `--version`
  names no GPU backend, so a CPU-only build reported every GPU backend true.
  A binary without the option or a report that does not parse = CPU-only, with a
  warning. Results cached per-binary-path for server lifetime. The Go server
  (`cmd/vmafx-mcp`) reads the same report through `pkg/scorebackend`.
- **Auto-dispatch backend identity comes from CLI receipt, never metric
  counts.** `vmaf` CLI writes top-level `backend_used` in JSON.
  `_infer_backend_from_payload` accepts concrete names from field, returns
  `unknown` when omitted or invalid. Metric counts (dated CPU 15 / CUDA 14 /
  SYCL 24) diagnostic observations only, not constants.
- **Default allowlist includes `/workspace/python/test/resource`
  alongside host-relative `<repo>/python/test/resource`** (ADR-0509,
  Bug B). Container [`dev/Containerfile`](../dev/Containerfile) bind-mounts
  repo root at `/workspace/`, placing Netflix golden YUVs at absolute path.
  `VMAF_MCP_ALLOW=/workspace/python/test/resource` extends default list.
- [ADR-0517](../docs/adr/0517-mcp-run-benchmark-repair.md) — `run_benchmark` repair.
- **HTTP transport optional dep group** (PR #1583, ADR-0701). `[http]` in
  `vmaf-mcp/pyproject.toml` (`aiohttp`, `prometheus-client`) preserved on rebase.
  `--transport http` in `src/vmaf_mcp/server.py::main()` dispatches to
  `src/vmaf_mcp/http_transport.py`. Dispatch block (`if args.transport == "http":`)
  returns before stdio loop. Production installs `[eval]` and `[http]`.
  Subtree fork-local; never merges upstream.
- **HTTP transport tests run in default dev environment** (ADR-1304).
  `[dev]` keeps `pytest-aiohttp` and `prometheus-client`:
  `tests/test_http_transport_round5.py` skips when absent. CI and nox
  install `[dev]`.
- **Python transport imports stay acyclic** (CodeQL alerts 917/918).
  `server.py` installs scoring adapter via `http_scoring.py`; `http_transport.py`
  consumes interface and never imports `server.py` directly. Startup in
  `server.py::main()`; validation, `ScoreRequest`, scoring, strict JSON
  behind shared interface. DAG pinned by `tests/test_import_graph.py`.
  Direct `run_http_server` callers inject runtime explicitly; injected object
  never replaces process-wide registry. Missing registration fails before
  socket bind. See ADR-1304.
- **MCP 2.x uses constructor-registered low-level handlers** (ADR-1129).
  `Server.list_tools()` / `Server.call_tool()` decorators and
  `Server.request_context` removed in mcp 2.1. Keep `_mcp_list_tools` and
  `_mcp_call_tool` registered via `Server(..., on_list_tools=..., on_call_tool=...)`;
  adapter translates dispatcher exceptions to `CallToolResult(isError=True)`,
  exposing request session via task-local context during call.
- **HTTP transport requires explicit env opt-in for 0.0.0.0 bind; auth
  defaults on** (ADR-0967). Default bind host `--transport http` = `127.0.0.1`
  (loopback). Listening on all interfaces requires `VMAFX_MCP_HTTP_BIND=0.0.0.0`.
  Auth enforced by default: if `VMAFX_MCP_HTTP_TOKEN` and
  `VMAFX_MCP_HTTP_NO_AUTH` unset, server rejects request with 401. Reverting
  `_resolve_bind_host()` returning "0.0.0.0" or removing security middleware from `_make_app()`
  re-introduces Round 26 audit finding A.1 vulnerabilities.
- **Required-argument tools rely on shared `_call_tool` `KeyError`→`ValueError` wrapper** (`tool 'X' missing required argument: 'key'`). Read required args with `arguments["key"]`, never add bespoke `if "key" not in arguments: raise ValueError("'key' is required ...")`
  `KeyError`→`ValueError` wrapper** (`tool 'X' missing required argument: 'key'`).
  Read required args with `arguments["key"]`, let missing key raise `KeyError`;
  never add bespoke `if "key" not in arguments: raise ValueError(...)`.
  Bespoke messages diverge from `test_call_tool_missing_*_raises_value_error`
  regex `missing required argument.*'key'`, breaking `MCP Smoke` lane
  (`probe_backend` 2026-06-20 fix).
- **Go↔Python byte-identical scoring surface** (ADR-1117 / #1240).
  Python (`server.py` `_scoring_extra_properties()` + `_extras_from_args` /
  `_build_vmaf_argv`) and Go (`cmd/vmafx-mcp/tools.go` + `impl.go`
  `parseScoreExtras` / `buildVmafArgv`) MUST declare identical `vmaf_score` /
  `vmaf_score_encoded` schema (property names, types, enums, defaults,
  selectors `--cpumask`, `--gpumask`, `--sycl_device`, `--hip_device`,
  `--metal_device`, `output_fmt`, `subsample`, tiny-AI flags) and identical
  `vmaf` CLI argv. Additions update both servers in lockstep. Pinned by
  `tests/test_parity_argv.py` and `TestGoAndPythonArgvParity`. Both include
  `python/test/resource/yuv` in allowed roots.
- **`tool-contract.json` = this server's tool list for Go parity.**
  `python3 -m vmaf_mcp.tool_contract --write` derives it from
  `_list_tools()` (names, property JSON types, required sets);
  `tests/test_tool_contract.py` fails while stale. Go
  `cmd/vmafx-mcp/tool_contract_test.go` reads it. Tool change -> regenerate,
  commit in same PR, add Go twin. Never hand-edit.
- **`_list_tools()` and `_scoring_extra_properties()` assembled from
  helpers (T-HISS-PY-COMPAT-2026-09-21).** Split for 60-LOC HISS-04 bound.
  Group functions concatenated in declaration order; catalogue byte-identical
  to single literal (ADR-1117 contract). When adding tool or scoring property,
  append to section group; keep Go server in lockstep.
