<!-- markdownlint-disable MD013 MD051 MD060 -->
# MCP servers

VMAFx ships three Model Context Protocol (MCP) servers that expose scoring
and tooling to LLM clients such as Claude Desktop and Cursor. Pick the one
that fits your deployment from the table below, then follow its install and
run section.

[Model Context Protocol](https://modelcontextprotocol.io) is a JSON-RPC
protocol that lets an LLM call declared tools with typed arguments. All three
servers are additive; running any combination at once is fine.

## Pick a server

| | Python `vmaf-mcp` | Go `vmafx-mcp` | Embedded (in `libvmaf`) |
| --- | --- | --- | --- |
| Install | `pip install -e mcp-server/vmaf-mcp` | `go build ./cmd/vmafx-mcp/` | `-Denable_mcp=true` at build time |
| Transports | stdio (default), HTTP | stdio (default), streamable HTTP | stdio, Unix socket, loopback SSE |
| Tools | 19 | 24 | 2 (`list_features`, `compute_vmaf`) |
| Configuration | environment variables and `--transport` / `--port` flags | environment variables only | meson options and C API |
| Needs a `vmaf` binary | yes (subprocess) | yes (subprocess; optional cgo for 2 tools) | no (in-process) |
| Use it when | you already run Python (deprecated, see below) | you want one static binary (recommended) | the host process needs an in-process control plane |
| Source | [`mcp-server/vmaf-mcp/`](../../mcp-server/vmaf-mcp/) | [`cmd/vmafx-mcp/`](../../cmd/vmafx-mcp/) | `core/src/mcp/` |

The embedded server is documented in [embedded.md](embedded.md). The rest of
this page covers the Python and Go servers; the Go server section starts at
[Go implementation](#go-implementation-vmafx-mcp).

!!! warning "The Python server is deprecated"
    [ADR-1229](../adr/1229-mcp-go-runtime.md) makes the Go binary the MCP
    server and deprecates the Python package. The Go binary is installed at
    `/usr/local/bin/vmafx-mcp` in the container images; attach with
    `docker exec -i vmaf-dev-mcp vmafx-mcp`. The Python package is kept as a
    reference implementation and as the schema-parity baseline; new tools go
    into `cmd/vmafx-mcp/` first.

!!! note "Tool counts"
    The Go server serves 24 tools and the Python server 19. The 15 classic
    tools and the 4 sidecar tools (`vmaf_per_shot`, `vmaf_roi`, `vmaf_bench`,
    `vmaf_vpl`) have identical names and schemas in both servers. The 5
    control-plane tools (`submit_job`, `get_job`, `cancel_job`, `list_jobs`,
    `vmaf_score_remote`) exist only in the Go server. The shared list is
    [`mcp-server/vmaf-mcp/tool-contract.json`](../../mcp-server/vmaf-mcp/tool-contract.json)
    (see [Tests](#tests)).

Use an MCP server when you want an LLM to:

- score a `(reference, distorted)` YUV pair and reason about the result,
- enumerate which VMAF models shipped with the build,
- probe which runtime backends (CPU / CUDA / SYCL / HIP / Metal) the local
  binary can dispatch to,
- run the Netflix benchmark harness and summarise the output,
- evaluate a tiny-AI ONNX regressor against a parquet feature cache
  on a deterministic split and report PLCC / SROCC / RMSE,
- rank several candidate tiny-AI models on the same split.

The servers exec the repo's own built `vmaf` binary under argv, never a
shell string, and refuse any file path that is not under an allowlisted
root. See [security](#security-model).

## Tool catalogue

The table lists all 24 tools. "Python" is yes for the 19 tools the Python
server also serves; full schemas, examples and error codes are in
[tools.md](tools.md).

| Tool | Python | Execution | Purpose |
| --- | --- | --- | --- |
| [`vmaf_score`](tools.md#vmaf_score) | yes | Subprocess (CLI); Go can use direct cgo (`VMAFX_MCP_DIRECT=1`) | Score one `(ref, dis)` YUV pair; return the full JSON report |
| [`vmaf_score_encoded`](tools.md#vmaf_score_encoded) | yes | Subprocess (`ffmpeg` + CLI) | Score a `(ref, dis)` encoded video pair via ffmpeg decode |
| [`list_models`](tools.md#list_models) | yes | Filesystem probe | Enumerate `.json` / `.pkl` / `.onnx` under `model/` |
| [`list_backends`](tools.md#list_backends) | yes | Subprocess (`vmaf` probe) | Report which backends the local `vmaf` binary was built with |
| [`probe_backend`](tools.md#probe_backend) | yes | Subprocess (`vmaf` probe) | Check whether a specific backend is runtime-healthy on this host |
| [`vmaf_version`](tools.md#vmaf_version) | yes | Subprocess (`vmaf -v`) | Return the version string reported by the local `vmaf` binary |
| [`run_benchmark`](tools.md#run_benchmark) | yes | Subprocess (`bench_all.sh`) | Run `testdata/bench_all.sh` on a pair |
| [`run_compare`](tools.md#run_compare) | yes | Subprocess (`vmaf-tune compare`) | Compare codec adapters at target VMAF scores |
| [`run_ladder`](tools.md#run_ladder) | yes | Subprocess (`vmaf-tune ladder`) | Generate a quality-ladder bitrate report |
| [`run_tune_per_shot`](tools.md#run_tune_per_shot) | yes | Subprocess (`vmaf-tune tune-per-shot`) | Per-shot CRF/QP tuning |
| [`eval_model_on_split`](tools.md#eval_model_on_split) | yes | Python: native; Go: native (libvmaf DNN API) | Evaluate a tiny-AI ONNX model on a parquet feature cache |
| [`compare_models`](tools.md#compare_models) | yes | Python: native; Go: native (libvmaf DNN API) | Rank several ONNX models on the same split by descending PLCC |
| [`list_extractors`](tools.md#list_extractors) | yes | Subprocess (`vmaf` probe) | Enumerate all feature extractors registered in libvmaf |
| [`describe_model`](tools.md#describe_model) | yes | Subprocess; Go can use direct cgo (`VMAFX_MCP_DIRECT=1`) | Return metadata for a VMAF model by name or path |
| [`describe_worst_frames`](tools.md#describe_worst_frames) | yes | Subprocess (CLI + VLM) | Score a pair, extract the N worst-VMAF frames as PNGs, and describe visible artefacts via a local VLM |
| [`vmaf_per_shot`](tools.md#vmaf_per_shot) | yes | Subprocess (`vmaf-perShot`) | Per-shot scoring |
| [`vmaf_roi`](tools.md#vmaf_roi) | yes | Subprocess (`vmaf_roi`) | Region-of-interest scoring |
| [`vmaf_bench`](tools.md#vmaf_bench) | yes | Subprocess (`vmaf_bench`) | Micro-benchmark harness |
| [`vmaf_vpl`](tools.md#vmaf_vpl) | yes | Subprocess (`vmaf_vpl`) | Intel VPL scoring tool |
| [`submit_job`](tools.md#submit_job) | Go only | gRPC (controller) | Submit a job to the controller |
| [`get_job`](tools.md#get_job) | Go only | gRPC (controller) | Fetch one job |
| [`cancel_job`](tools.md#cancel_job) | Go only | gRPC (controller) | Cancel a job |
| [`list_jobs`](tools.md#list_jobs) | Go only | gRPC (controller) | Snapshot of jobs |
| [`vmaf_score_remote`](tools.md#vmaf_score_remote) | Go only | gRPC (`vmafx-server`) | Score through a remote scoring service |

All tools return a single `TextContent` message whose body is a JSON
document. On error the body is `{"error": "<message>"}` with the same
shape so the client can always `json.loads()` the response.

### Execution mechanics

| Group | Execution | Tools |
| --- | --- | --- |
| Process-based | the server spawns `vmaf`, `vmaf-tune`, `ffmpeg` or a sidecar binary | most tools |
| Direct cgo (Go only, opt-in) | in-process libvmaf call when `VMAFX_MCP_DIRECT=1` | `vmaf_score`, `describe_model` |
| gRPC (Go only) | call to the controller or `vmafx-server` | `submit_job`, `get_job`, `cancel_job`, `list_jobs`, `vmaf_score_remote` |

Migrating the remaining process-based tools to direct calls is tracked in
[ADR-0704](../adr/0704-vmafx-mcp-go-port.md) and
[ADR-0931](../adr/0931-mcp-cgo-direct-replace-subprocess.md). The embedded C
server (`core/src/mcp/dispatcher.c`, ADR-0209) runs entirely in-process,
without child processes, and serves exactly two tools: `list_features` and
`compute_vmaf`.

## Install (Python server)

From a checkout of the repo:

1. Build `vmaf` from the repository root (Meson + Ninja; see
   [building from source](../getting-started/index.md#build-from-source-any-platform)).

    ```bash
    meson setup build core -Denable_cuda=false -Denable_sycl=false
    ninja -C build
    ```

2. Install the MCP server package.

    ```bash
    cd mcp-server/vmaf-mcp
    pip install -e .
    ```

3. Optional: pull in the ML dependencies for `eval_model_on_split` and
   `compare_models`.

    ```bash
    pip install -e '.[eval]'
    ```

The server lands as `vmaf-mcp` on your PATH. It finds the `vmaf` CLI in this
order: `VMAF_BIN`, `/usr/local/bin/vmaf`, `<repo>/core/build/tools/vmaf`,
`<repo>/build/tools/vmaf`.

### Testing

The test suite is in `mcp-server/vmaf-mcp/tests/`. The package
`pyproject.toml` sets `pythonpath = ["src"]` under
`[tool.pytest.ini_options]`, so pytest run from the package directory finds
the in-tree `vmaf_mcp` module without an editable installation:

```bash
cd mcp-server/vmaf-mcp
pytest
```

## Run (Python server)

```bash
# Default stdio transport, as used by Claude Desktop and Cursor
vmaf-mcp
```

No network ports are opened. The server reads JSON-RPC requests from stdin
and writes responses to stdout; diagnostic logs go to stderr. For the HTTP
mode see [http-transport.md](http-transport.md).

### Claude Desktop configuration

Drop this into
`~/Library/Application Support/Claude/claude_desktop_config.json`
(macOS) or `%APPDATA%/Claude/claude_desktop_config.json` (Windows):

```json
{
  "mcpServers": {
    "vmaf-local": {
      "command": "vmaf-mcp",
      "env": {
        "VMAF_BIN": "/home/you/dev/vmaf/build/tools/vmaf",
        "VMAF_MCP_ALLOW": "/home/you/yuv-corpus:/home/you/renders"
      }
    }
  }
}
```

A complete example covering the Docker image variant lives in
[mcp-server/vmaf-mcp/claude-desktop-config-example.json](../../mcp-server/vmaf-mcp/claude-desktop-config-example.json).

## Environment variables

The tool-handler variables below are read by both servers unless the last
column says otherwise. The variables only the Go server reads (its transport,
the control-plane tools, the direct scoring path) are in its
[environment table](#environment-vmafx-mcp).

| Variable | Purpose | Default | Server |
| --- | --- | --- | --- |
| `VMAF_BIN` | Absolute path to the `vmaf` CLI binary | search order above | both |
| `VMAF_MCP_ALLOW` | Colon-separated extra roots under which file paths are accepted | empty (built-in roots only) | both |
| `VMAF_MCP_ASYNC` | AnyIO backend (`asyncio` / `trio`) | `asyncio` | Python |
| `VMAF_MCP_MAX_CONCURRENT` | Cap on concurrent `vmaf` subprocesses | `8` | Python |
| `VMAF_MCP_SUBPROCESS_TIMEOUT_S` | Wall-clock timeout per subprocess call, in seconds | `600` | Python |
| `VMAF_TUNE_BIN` | Path to the `vmaf-tune` binary | search order | both |
| `VMAF_ROOT` | Data root holding the fixture YUVs that `run_benchmark` uses | repo root if it holds the fixtures, else `/workspace` | both |
| `VMAF_BENCH_OUTDIR` | Output directory of `testdata/bench_all.sh` (see [tools.md](tools.md#run_benchmark)) | `/tmp/vmaf-bench-<pid>` | both |
| `VMAF_PER_SHOT_BIN`, `VMAF_ROI_BIN`, `VMAF_BENCH_BIN`, `VMAF_VPL_BIN` | Override the path of each sidecar binary | next to `vmaf`, then `/usr/local/bin`, then the build trees | both |

The HTTP security variables (`VMAFX_MCP_HTTP_TOKEN`, `..._NO_AUTH`,
`..._BIND`, `..._TLS_CERT`, `..._TLS_KEY`) are documented in
[http-transport.md](http-transport.md).

## Security model

The server is meant to run on the user's own machine, driven by a local LLM
client. Because the LLM could craft input that tries to read arbitrary host
paths, the server enforces a path allowlist.

Built-in roots, always allowed:

| Root | Note |
|---|---|
| `testdata/` | fork-added fixtures |
| `python/test/resource/` | includes `python/test/resource/yuv/` |
| `model/` | shipped models |
| `/workspace/python/test/resource` | the `vmaf-dev-mcp` container mount |

Extra roots can be added with `VMAF_MCP_ALLOW=<abs-path>[:<abs-path>...]`.

Any tool argument that names a file (`ref`, `dis`, `model`, `features`, each
member of `models`) is resolved with `Path.resolve()` and rejected unless it
lands under one of the allowed roots **and** refers to an existing regular
file. `..` segments and symlinks that escape the allowlist are rejected by
resolution.

The underlying CLI is exec'd with an `argv` list, never a shell string, so
there is no pathway for shell-metacharacter injection.

!!! note "Control-plane tools"
    The Go-only gRPC tools do not use this allowlist: their `reference` and
    `distorted` paths live on the worker node, so only the path shape is
    checked. See [tools.md](tools.md#path-arguments-are-resolved-remotely).

See also [ai/security.md](../ai/security.md) for the tiny-AI-specific
hardening (ONNX operator allowlist, model size cap).

## When not to use the MCP server

- **Bulk scoring in a pipeline** — use the
  [`vmaf` CLI directly](../usage/cli.md). MCP is request/response; the
  CLI streams pictures and does not pay JSON-RPC overhead per frame.
- **Integration into your own code** — use the
  [C API](../api/index.md) or the
  [Python bindings](../usage/python.md) for an in-process surface.
- **CI checks** — the [Docker image](../usage/docker.md) is a better
  fit than stdio-attached MCP.

MCP shines when the caller is an LLM that benefits from a tool-calling
interface with declared schemas and a JSON-shaped response.

## Go implementation — `vmafx-mcp`

`vmafx-mcp` is a single static Go binary that serves the 19 tools of the
Python server with byte-for-byte schema parity (ADR-0704) plus the 5
control-plane tools. It is the recommended implementation for deployments
that cannot install a Python environment.

### Build

The binary needs no runtime dependencies other than the `vmaf` CLI binary
(resolved via `VMAF_BIN` or the standard search order). From the repository
root, with Go 1.27 or newer:

```bash
go build -o vmafx-mcp ./cmd/vmafx-mcp/
```

### Run

The Go binary runs on the golusoris fx framework (ADR-1119) and is configured
entirely through environment variables; it has no CLI flags.

```bash
# Default stdio transport, a drop-in replacement for vmaf-mcp
vmafx-mcp

# Streamable-HTTP transport on the default address :3000
VMAFX_MCP_TRANSPORT=http vmafx-mcp

# Streamable-HTTP transport on a custom address
VMAFX_MCP_TRANSPORT=http VMAFX_MCP_HTTP_ADDR=:8080 vmafx-mcp
```

The HTTP transport serves the MCP streamable-HTTP protocol at the listen
address. It does not serve the REST routes of the Python HTTP mode; see
[http-transport.md](http-transport.md) for the comparison and the security
variables.

!!! warning "Migration (ADR-1119)"
    The pre-framework binary used `vmafx-mcp --transport http --port 3000`.
    Replace `--transport <t>` with `VMAFX_MCP_TRANSPORT=<t>` and
    `--port <N>` with `VMAFX_MCP_HTTP_ADDR=:<N>`. The value is a full listen
    address, not a bare port. The historical default port `3000` is kept as
    the default address `:3000`.

### Environment (`vmafx-mcp`)

Every variable the Go binary reads, from the
[platform definition](../development/api-generation.md#environment-of-the-go-binaries):

<!-- BEGIN GENERATED: vmafx-api environment vmafx-mcp (scripts/codegen/vmafx-api.py) -->

| Variable | Key | Type | Default | Chart value | Description |
|---|---|---|---|---|---|
| `VMAF_BIN` | read directly | path | `/usr/local/bin/vmaf`, then the build trees |  | Path of the `vmaf` CLI behind the scoring tools, looked up on every tool call. |
| `VMAFX_CONTROLLER_ADDR` | read directly | `host:port` | `localhost:9090` |  | gRPC address of `vmafx-controller` for the control-plane tools, read on every call. |
| `VMAFX_CONTROLLER_TOKEN` | read directly | string | _(unset)_ |  | Secret. Bearer token sent with every controller call of the control-plane tools (plaintext transport). With auth on, `get_job` and `list_jobs` need `vmafx:reader`, `submit_job` and `cancel_job` `vmafx:writer`. |
| `VMAFX_MCP_TRANSPORT` | `mcp.transport` | string | `stdio` |  | `stdio` or `http` (streamable HTTP); anything else stops the server. |
| `VMAFX_MCP_HTTP_ADDR` | `mcp.http.addr` | `host:port` | `:3000` |  | Listen address of the HTTP transport; an address without a host takes `VMAFX_MCP_HTTP_BIND`. |
| `VMAFX_MCP_HTTP_BIND` | read directly | host | `127.0.0.1` |  | Host of a listen address that names none; `0.0.0.0` listens on all interfaces ([ADR-0967](../adr/0967-mcp-http-transport-security-hardening.md)). |
| `VMAFX_MCP_HTTP_TOKEN` | read directly | string | _(unset: every request is refused unless `VMAFX_MCP_HTTP_NO_AUTH=1`)_ |  | Secret. Bearer token the HTTP transport requires in `Authorization: Bearer <token>`. |
| `VMAFX_MCP_HTTP_NO_AUTH` | read directly | `1` | off |  | `1` turns the HTTP transport's authentication off; the server logs a warning. |
| `VMAFX_MCP_DIRECT` | read directly | `1` | off |  | `1` scores through libvmaf by cgo instead of a `vmaf` subprocess ([ADR-0931](../adr/0931-mcp-cgo-direct-replace-subprocess.md)); read on every tool call. |
| `VMAFX_SERVER_ADDR` | read directly | `host:port` | `localhost:9090` |  | gRPC address of `vmafx-server` for `vmaf_score_remote`. |
| `VMAFX_GRPC_TIMEOUT` | read directly | seconds | `30` |  | Deadline of each control-plane RPC; a value that is not a positive integer keeps the default. |
| `VMAF_MCP_ALLOW` | read directly | path list | built-in roots |  | Additional roots, separated by the OS path-list separator, under which file paths are accepted. |
| `VMAF_PER_SHOT_BIN` | read directly | path | next to `vmaf`, then `/usr/local/bin` and the build trees |  | Binary of the `vmaf-perShot` tool. |
| `VMAF_ROI_BIN` | read directly | path | next to `vmaf`, then `/usr/local/bin` and the build trees |  | Binary of the `vmaf_roi` tool. |
| `VMAF_BENCH_BIN` | read directly | path | next to `vmaf`, then `/usr/local/bin` and the build trees |  | Binary of the `vmaf_bench` tool. |
| `VMAF_VPL_BIN` | read directly | path | next to `vmaf`, then `/usr/local/bin` and the build trees |  | Binary of the `vmaf_vpl` tool. |
| `VMAF_ROOT` | read directly | path | the repository root when it holds the fixtures, else `/workspace` |  | Data root of the fixture clips `run_benchmark` uses. |
| `VMAF_TUNE_BIN` | read directly | path | `vmaf-tune` on `PATH`, else the repository's |  | `vmaf-tune` binary of the tuning tools. |
| `VMAFX_LOG_LEVEL` | `log.level` | string | `info` |  | Log level: `debug`, `info`, `warn` or `error`, any case; an unknown value gives `info`. |
| `VMAFX_LOG_FORMAT` | `log.format` | string | `auto` |  | Log handler: `auto` (tint on a terminal, else JSON), `tint` or `json`; logs go to stderr. |
| `VMAFX_OTEL_ENABLED` | `otel.enabled` | bool | `true` |  | OpenTelemetry master switch; `false` installs no-op providers even with an endpoint. |
| `VMAFX_OTEL_ENDPOINT` | `otel.endpoint` | `host:port` | _(unset)_ |  | OTLP/gRPC collector (`otel-collector:4317`); wins over `OTEL_EXPORTER_OTLP_ENDPOINT`. Neither set: no export ([OpenTelemetry](../observability/otel.md)). |
| `VMAFX_OTEL_INSECURE` | `otel.insecure` | bool | `true` |  | Plaintext gRPC to the collector; `false` dials with TLS. |
| `VMAFX_OTEL_SERVICE_NAME` | `otel.service.name` | string | `OTEL_SERVICE_NAME`, else the binary name |  | `service.name` resource attribute. |
| `VMAFX_OTEL_SERVICE_VERSION` | `otel.service.version` | string | the build version |  | `service.version` resource attribute. |
| `VMAFX_OTEL_SERVICE_NAMESPACE` | `otel.service.namespace` | string | _(unset)_ |  | `service.namespace` resource attribute. |
| `VMAFX_OTEL_SAMPLE_RATIO` | `otel.sample.ratio` | number | `1.0` |  | Parent-based trace sample ratio in `[0, 1]`; `OTEL_TRACES_SAMPLER` and its argument are not read. |
| `VMAFX_OTEL_EXPORT_TRACES` | `otel.export.traces` | bool | `true` |  | Export traces. |
| `VMAFX_OTEL_EXPORT_METRICS` | `otel.export.metrics` | bool | `true` |  | Export metrics. |
| `VMAFX_OTEL_EXPORT_LOGS` | `otel.export.logs` | bool | `true` |  | Export the logs signal; application logs are not bridged to it today. |
| `OTEL_SERVICE_NAME` | read directly | string | _(unset)_ |  | `service.name` when `VMAFX_OTEL_SERVICE_NAME` is unset (OTel standard). |
| `OTEL_SDK_DISABLED` | read directly | string | _(unset)_ |  | `true` (exactly) installs no-op providers (OTel standard). |
| `OTEL_EXPORTER_OTLP_ENDPOINT` | read directly | URL | _(unset)_ |  | Collector as a URL (`http://host:4317`) when `VMAFX_OTEL_ENDPOINT` is unset; set, export is on (OTel standard). |
| `OTEL_EXPORTER_OTLP_TRACES_ENDPOINT` | read directly | URL | _(unset)_ |  | Per-signal collector URL for traces; set, export is on (OTel standard). |
| `OTEL_EXPORTER_OTLP_METRICS_ENDPOINT` | read directly | URL | _(unset)_ |  | Per-signal collector URL for metrics; set, export is on (OTel standard). |
| `OTEL_EXPORTER_OTLP_LOGS_ENDPOINT` | read directly | URL | _(unset)_ |  | Per-signal collector URL for logs; set, export is on (OTel standard). |
| `POD_NAME` | read directly | string | _(unset)_ |  | Pod name (Kubernetes downward API), added to log lines and OTel resources as `k8s.pod.name`. |
| `POD_NAMESPACE` | read directly | string | _(unset)_ |  | Pod namespace, added as `k8s.namespace.name`. |
| `POD_IP` | read directly | string | _(unset)_ |  | Pod IP, added as `k8s.pod.ip`. |
| `NODE_NAME` | read directly | string | _(unset)_ |  | Kubernetes node name, added as `k8s.node.name`. |
| `SERVICE_ACCOUNT` | read directly | string | _(unset)_ |  | Service account name, added as `k8s.serviceaccount.name`. |

<!-- END GENERATED: vmafx-api environment vmafx-mcp -->

### Claude Desktop configuration (Go binary)

```json
{
  "mcpServers": {
    "vmafx-local": {
      "command": "/path/to/vmafx-mcp",
      "env": {
        "VMAF_BIN": "/home/you/dev/vmaf/build/tools/vmaf",
        "VMAF_MCP_ALLOW": "/home/you/yuv-corpus"
      }
    }
  }
}
```

### Differences from the Python server

| Feature | Python (`vmaf-mcp`) | Go (`vmafx-mcp`) |
| --- | --- | --- |
| Tool count | 19 | 24 (19 shared + 5 control-plane) |
| Shared tool names / schemas | Reference | Byte-for-byte parity |
| Transport | stdio (default), HTTP (PR #1583); `--transport` / `--port` flags | stdio (default), streamable HTTP; env vars `VMAFX_MCP_TRANSPORT` / `VMAFX_MCP_HTTP_ADDR`, no flags (ADR-1119) |
| VLM descriptions (`describe_worst_frames`) | A local ONNX Runtime GenAI vision model (`[vlm]` extra, `VMAF_MCP_VLM_MODEL`) | Returns a placeholder |
| `eval_model_on_split`, `compare_models` | Native Python (onnxruntime, pandas, scipy) | Native Go, no Python: parquet via `parquet-go`, statistics in `pkg/modeleval`, ONNX via libvmaf's `vmaf_dnn_session_*` API |
| Binary size | about 50 MB Python environment | about 10 MB static binary |
| Startup time | about 300 ms (Python import) | about 10 ms |

The size and startup figures are indicative, not a recorded measurement.
The Go evaluation tools need a libvmaf built with `-Denable_dnn`; otherwise
they return a clear "built without DNN support" error, mirroring Python's
missing-`[eval]`-extra behaviour.

### Go environment variables

Every variable `vmafx-mcp` reads, the framework's `VMAFX_LOG_*` and
`VMAFX_OTEL_*` keys included, is in its
[environment table](#environment-vmafx-mcp). All framework logging goes to
**stderr**, so the stdio JSON-RPC stream on stdout stays uncorrupted.

### Startup contract: all tools or none

`vmafx-mcp` serves the complete 24-tool surface or it does not start. If a
tool's input schema fails to serialise at startup, the process writes the
failing tool's name to stderr and exits non-zero.

!!! note "Why the server refuses to start"
    The alternative would be to start anyway with that one tool's schema
    replaced by a permissive `{"type": "object"}`. That server would look
    identical over the wire to a healthy one, while the affected tool silently
    accepted every argument map, including ones it cannot run. A listed tool is
    therefore a tool whose declared arguments were validated as written.

The failure is a defect in the server's own schema literals, not something an
operator can configure, so there is no flag to relax it. If `vmafx-mcp` exits
at startup with `registering the MCP tool surface: tool "<name>": ...`, report
the tool name; nothing in the environment can cause or fix it.

### Tests

```bash
go test ./cmd/vmafx-mcp/ -v
```

`TestToolListMatchesPython` and `TestToolSchemasMatchPython` compare the
served tools with the Python server's tool list, read from
[`mcp-server/vmaf-mcp/tool-contract.json`](../../mcp-server/vmaf-mcp/tool-contract.json):
every Python tool must be served with the same property names, JSON types and
required arguments, and every other tool must be one of the five declared
Go-only tools. They need neither Python nor the test YUVs (the package still
links libvmaf, like every `go test` here). `TestVmafScoreTool` and
`TestGoVsPythonOutputParity` require the Netflix golden YUVs and the `vmaf`
binary; they skip automatically when these are absent.

The contract file is written from the Python server's `_list_tools()`, never
by hand. After adding or changing a tool in `server.py`, regenerate it and
commit it with the change:

```bash
PYTHONPATH=mcp-server/vmaf-mcp/src python3 -m vmaf_mcp.tool_contract --write
```

`mcp-server/vmaf-mcp/tests/test_tool_contract.py` fails while the file is
stale (`python3 -m vmaf_mcp.tool_contract --check` gives the same answer), and
a change to the file runs the Go checks in CI, so the Go tests then name the
tool the Go server still lacks.

## Related

- [Tool reference](tools.md) — request/response schemas and error codes
  for every tool.
- [Backend discovery and default allowlist](backends.md) — how
  `list_backends` probes compiled-in GPU runtimes and which paths the
  server accepts without `VMAF_MCP_ALLOW` (ADR-0511).
- [Embedded server](embedded.md) — the in-process server inside libvmaf.
- [ADR-0100](../adr/0100-project-wide-doc-substance-rule.md) — the
  per-surface doc bar this page satisfies (MCP tool: what / schema /
  allowed paths / example / error codes).
- [ADR-0704](../adr/0704-vmafx-mcp-go-port.md) — decision record for the Go
  port.
- [mcp-server/vmaf-mcp/README.md](../../mcp-server/vmaf-mcp/README.md) —
  short-form README kept alongside the Python code.
