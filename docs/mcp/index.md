<!-- markdownlint-disable MD013 MD051 MD060 -->
# MCP server — `vmaf-mcp`

The VMAFX fork ships **three** MCP surfaces:

1. **External Python MCP server** (`vmaf-mcp`) —
   wraps the `vmaf` CLI via subprocess. Stable; in production use.
   Lives in [`mcp-server/vmaf-mcp/`](../../mcp-server/vmaf-mcp/).
2. **External Go MCP server** (`vmafx-mcp`) — single static binary,
   same 15 tools, byte-for-byte schema parity with the Python server.
   Lives in [`cmd/vmafx-mcp/`](../../cmd/vmafx-mcp/). Stage 1 (Python
   preserved alongside). See [Go implementation](#go-implementation-vmafx-mcp) below.
3. **Embedded MCP server inside libvmaf** — runs in-process on
   the host that loaded `libvmaf.so`; serves stdio, UDS, and
   loopback SSE transports with `list_features` and `compute_vmaf`.
   It is the right surface when an embedding host needs an
   in-process control plane rather than a child `vmaf` process.
   Model hot-swap and frame-boundary SPSC draining remain future
   work. See [`docs/mcp/embedded.md`](embedded.md) for build flags,
   transport limits, and the C API reference.

All three surfaces are additive; running any combination at once is fine.
This document covers surfaces 1 and 2. See [embedded.md](embedded.md) for surface 3.

`vmaf-mcp` is a [Model Context Protocol](https://modelcontextprotocol.io)
server that exposes the VMAFx fork's scoring CLI to LLM tooling
(Claude Desktop, Cursor, custom MCP clients) over JSON-RPC on stdio.
It lives in [mcp-server/vmaf-mcp/](../../mcp-server/vmaf-mcp/).

Use it when you want an LLM to:

- score a `(reference, distorted)` YUV pair and reason about the result,
- enumerate which VMAF models shipped with the build,
- probe which runtime backends (CPU / CUDA / SYCL / Vulkan / HIP / Metal) the local
  binary can dispatch to,
- run the Netflix benchmark harness and summarise the output,
- evaluate a tiny-AI ONNX regressor against a parquet feature cache
  on a deterministic split and report PLCC / SROCC / RMSE,
- rank several candidate tiny-AI models on the same split.

The server exec's the repo's own built `vmaf` binary under argv — it
never passes a shell string — and refuses any file path that is not
under an allowlisted root. See [security](#security-model) below.

## Tool catalogue

| Tool | Execution | Purpose | Detail |
|---|---|---|---|
| `vmaf_score` | Subprocess (CLI) / Direct Cgo (`VMAFX_MCP_DIRECT=1`) | Score one `(ref, dis)` YUV pair; return the full JSON report | [tools.md#vmaf_score](tools.md#vmaf_score) |
| `vmaf_score_encoded` | Subprocess (`ffmpeg` + CLI) | Score a `(ref, dis)` encoded video pair via ffmpeg decode | [tools.md#vmaf_score_encoded](tools.md#vmaf_score_encoded) |
| `list_models` | Subprocess / filesystem probe | Enumerate `.json` / `.pkl` / `.onnx` under `model/` | [tools.md#list_models](tools.md#list_models) |
| `list_backends` | Subprocess (`vmaf` probe) | Report which backends the local `vmaf` binary was built with | [tools.md#list_backends](tools.md#list_backends) |
| `probe_backend` | Subprocess (`vmaf` probe) | Check whether a specific backend is runtime-healthy on this host | [tools.md#probe_backend](tools.md#probe_backend) |
| `vmaf_version` | Subprocess (`vmaf -v`) | Return the version string reported by the local `vmaf` binary | [tools.md#vmaf_version](tools.md#vmaf_version) |
| `run_benchmark` | Subprocess (`bench_all.sh`) | Run `testdata/bench_all.sh` on a pair | [tools.md#run_benchmark](tools.md#run_benchmark) |
| `run_compare` | Subprocess (`vmaf-tune compare`) | Wrap `vmaf-tune compare`: compare codec adapters at target VMAF scores | [tools.md#run_compare](tools.md#run_compare) |
| `run_ladder` | Subprocess (`vmaf-tune ladder`) | Wrap `vmaf-tune ladder`: generate a quality-ladder bitrate report | [tools.md#run_ladder](tools.md#run_ladder) |
| `run_tune_per_shot` | Subprocess (`vmaf-tune per-shot`) | Wrap `vmaf-tune per-shot`: per-shot CRF/QP tuning | [tools.md#run_tune_per_shot](tools.md#run_tune_per_shot) |
| `eval_model_on_split` | Subprocess / Native Go ORT | Evaluate a tiny-AI ONNX model on a parquet feature cache | [tools.md#eval_model_on_split](tools.md#eval_model_on_split) |
| `compare_models` | Subprocess / Native Go ORT | Rank several ONNX models on the same split by descending PLCC | [tools.md#compare_models](tools.md#compare_models) |
| `list_extractors` | Subprocess (`vmaf` probe) | Enumerate all `VmafFeatureExtractor` implementations in `core/src/feature/` | [tools.md#list_extractors](tools.md#list_extractors) |
| `describe_model` | Subprocess / Direct Cgo (`VMAFX_MCP_DIRECT=1`) | Return metadata for a VMAF model by name or path | [tools.md#describe_model](tools.md#describe_model) |
| `describe_worst_frames` | Subprocess (CLI + VLM) | Score a pair, extract the N worst-VMAF frames as PNGs, and describe visible artefacts via a local VLM | [tools.md#describe_worst_frames](tools.md#describe_worst_frames) |

> **Execution mechanics (GAP-MCP-SUBPROCESS-VS-CGO-DIRECT).** 13 of the 15 tools in the Go server (`cmd/vmafx-mcp`)
> currently shell out to the `vmaf` / `vmaf-tune` CLI or external processes. Only `vmaf_score` and `describe_model`
> offer a direct in-process Cgo path when `VMAFX_MCP_DIRECT=1` is set. Full migration of the remaining 13 tools to direct
> Cgo/in-process library calls is tracked under the Go migration plan (ADR-0704 / ADR-0931).
>
> In contrast, the embedded C MCP server inside `libvmaf` (`core/src/mcp/dispatcher.c`, ADR-0209) executes entirely
> in-process without any child subprocesses, currently serving exactly 2 tools: `list_features` and `compute_vmaf` (GAP-MCP-C-EMBEDDED-ONLY-2-TOOLS).

All tools return a single `TextContent` message whose body is a JSON
document. On error the body is `{"error": "<message>"}` with the same
shape so the client can always `json.loads()` the response.

## Install

From a checkout of the repo:

```bash
# 1. build vmaf (Meson + Ninja; see CLAUDE.md §2)
meson setup build -Denable_cuda=false -Denable_sycl=false
ninja -C build

# 2. install the MCP server package
cd mcp-server/vmaf-mcp
pip install -e .

# optional: pull in ML deps for eval_model_on_split / compare_models
pip install -e '.[eval]'
```

The server binary lands as `vmaf-mcp` on your PATH. It expects to find
the vmaf CLI at `build/tools/vmaf` relative to the repo root. Override
with `VMAF_BIN=/abs/path/to/vmaf`.

## Run

```bash
# Default stdio transport — what Claude Desktop / Cursor use
vmaf-mcp
```

No network ports are opened. The server reads JSON-RPC requests from
stdin and writes responses to stdout; diagnostic logs go to stderr.

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

| Variable           | Purpose                                                                            | Default                                 |
|--------------------|------------------------------------------------------------------------------------|-----------------------------------------|
| `VMAF_BIN`         | Absolute path to the `vmaf` CLI binary                                             | `<repo>/build/tools/vmaf`               |
| `VMAF_MCP_ALLOW`   | Colon-separated extra roots under which file paths are accepted                    | (empty — only built-in roots)           |
| `VMAF_MCP_ASYNC`   | AnyIO backend (`asyncio` / `trio`)                                                 | `asyncio`                               |

## Security model

The server is meant to run on the user's own machine, driven by a local
LLM client. Even so, any JSON-RPC input could be crafted by the LLM to
try to coerce the server into reading arbitrary host paths — so the
server enforces a path allowlist:

- Built-in roots (always allowed):
  - `testdata/`
  - `python/test/resource/`
  - `model/`
- Extra roots can be added via `VMAF_MCP_ALLOW=<abs-path>[:<abs-path>...]`.

Any tool argument that names a file (`ref`, `dis`, `model`, `features`,
each member of `models`) is resolved with `Path.resolve()` and rejected
unless it lands under one of the allowed roots **and** refers to an
existing regular file. `..` segments and symlinks that escape the
allowlist are rejected by resolution.

The underlying CLI is exec'd with an `argv` list — never a shell
string — so there is no pathway for shell-metacharacter injection.

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

MCP shines when the caller is an LLM that benefits from having a
tool-calling interface with declared schemas and a JSON-shaped response.

## Go implementation — `vmafx-mcp`

`vmafx-mcp` is a single static Go binary that exposes the same 15 MCP tools as
the Python server with byte-for-byte schema parity (ADR-0704). It is the
recommended implementation for deployments that cannot install a Python
environment.

### Build

```bash
# From the repository root (Go 1.25+)
go build -o vmafx-mcp ./cmd/vmafx-mcp/
```

The binary has no runtime dependencies other than the `vmaf` CLI binary
(resolved via `VMAF_BIN` or the standard search order).

### Run

The Go binary is wired on the golusoris fx framework (ADR-1119) and is
configured entirely through environment variables — there are **no CLI
flags**. Transport selection moved from the removed `--transport` / `--port`
flags to `VMAFX_MCP_TRANSPORT` / `VMAFX_MCP_HTTP_ADDR`.

```bash
# Default stdio transport — drop-in replacement for vmaf-mcp
vmafx-mcp

# Streamable-HTTP transport on the default address :3000
VMAFX_MCP_TRANSPORT=http vmafx-mcp

# Streamable-HTTP transport on a custom address
VMAFX_MCP_TRANSPORT=http VMAFX_MCP_HTTP_ADDR=:8080 vmafx-mcp
```

> **Migration (ADR-1119).** The pre-framework binary used
> `vmafx-mcp --transport http --port 3000`. Replace
> `--transport <t>` with `VMAFX_MCP_TRANSPORT=<t>` and
> `--port <N>` with `VMAFX_MCP_HTTP_ADDR=:<N>` (a full listen address,
> not a bare port). The historical default port `3000` is preserved as the
> default address `:3000`.

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
|---|---|---|
| Tool names / schemas | Reference | Byte-for-byte parity |
| Transport | stdio (default), HTTP (PR #1583); `--transport` / `--port` flags | stdio (default), HTTP; selected via `VMAFX_MCP_TRANSPORT` / `VMAFX_MCP_HTTP_ADDR` env vars (no flags, ADR-1119) |
| VLM descriptions (`describe_worst_frames`) | SmolVLM / Moondream2 when `[vlm]` extras installed | Returns placeholder; Stage 2 will add a native VLM bridge |
| `eval_model_on_split` / `compare_models` | Native Python (onnxruntime, pandas, scipy) | Native Go; no Python. Parquet via `parquet-go`, statistics in `pkg/modeleval`, ONNX via libvmaf's own `vmaf_dnn_session_*` API. Needs a libvmaf built with `-Denable_dnn`; otherwise returns a clear "built without DNN support" error, mirroring Python's missing-`[eval]`-extra behaviour |
| Binary size | ~50 MB Python env | ~10 MB static binary |
| Startup time | ~300 ms (Python import) | ~10 ms |

### Environment variables

Tool-handler variables are the same as the Python server (`VMAF_BIN`,
`VMAF_MCP_ALLOW`, plus `VMAFX_MCP_DIRECT=1` to opt into the direct cgo scoring
path — ADR-0931). On top of those, the fx framework (ADR-1119) adds the
config-driven keys below. Config uses the `VMAFX_` env prefix with a `.`
koanf delimiter, so every `_` in the variable name becomes a `.` in the
koanf key:

| Variable | koanf key | Default | Purpose |
|---|---|---|---|
| `VMAFX_MCP_TRANSPORT` | `mcp.transport` | `stdio` | Transport: `stdio` or `http`. |
| `VMAFX_MCP_HTTP_ADDR` | `mcp.http.addr` | `:3000` | HTTP listen address (used only when transport is `http`). Full address (`:3000`), not a bare port. |
| `VMAFX_LOG_LEVEL` | (bridged to `LOG_LEVEL`) | `INFO` | slog level. golusoris#234: bridged to the bare `LOG_LEVEL` the v0.4.0 log module reads. |
| `VMAFX_LOG_FORMAT` | (bridged to `LOG_FORMAT`) | auto | Log handler (`auto`/`tint`/`json`). |

All framework logging is written to **stderr** so the stdio JSON-RPC stream on
stdout stays uncorrupted.

### Tests

```bash
go test ./cmd/vmafx-mcp/ -v
```

`TestToolListMatchesPython` and `TestToolSchemasMatchPython` run without any
external dependencies. `TestVmafScoreTool` and `TestGoVsPythonOutputParity`
require the Netflix golden YUVs and the `vmaf` binary (skipped automatically
when absent).

## Related

- [Tool reference](tools.md) — request/response schemas and error codes
  for every tool.
- [Backend discovery and default allowlist](backends.md) — how
  `list_backends` probes compiled-in GPU runtimes and which paths the
  server accepts without `VMAF_MCP_ALLOW` (ADR-0511).
- [ADR-0100](../adr/0100-project-wide-doc-substance-rule.md) — the
  per-surface doc bar this page satisfies (MCP tool: what / schema /
  allowed paths / example / error codes).
- [ADR-0704](../adr/0704-vmafx-mcp-go-port.md) — decision record for the Go port.
- [mcp-server/vmaf-mcp/README.md](../../mcp-server/vmaf-mcp/README.md) —
  short-form README kept alongside the Python code.
