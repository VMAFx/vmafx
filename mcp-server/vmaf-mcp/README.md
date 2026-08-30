<!-- markdownlint-disable MD060 -->
# vmaf-mcp

MCP (Model Context Protocol) server that exposes the Lusoris VMAF fork's
scoring CLI to LLM tooling via JSON-RPC over stdio.

## Tools

| Tool                    | Description                                                                        |
| ----------------------- | ---------------------------------------------------------------------------------- |
| `vmaf_score`            | Score a (ref, dis) raw YUV pair. Returns the full JSON report.                     |
| `vmaf_score_encoded`    | Score encoded video (MP4/MKV/Y4M/…) — decodes via ffmpeg, then scores. (ADR-0608) |
| `list_models`           | Enumerate models under `model/` (`.json`, `.pkl`, `.onnx`).                        |
| `list_backends`         | Report which backends (`cpu`/`cuda`/`sycl`/`hip`/`metal`) are compiled in. |
| `probe_backend`         | Runtime health check: compiled-in vs driver-functional distinction. (ADR-0608)     |
| `vmaf_version`          | Return binary path, version string, and build flags. (ADR-0608)                    |
| `run_benchmark`         | Run `testdata/bench_all.sh` on the built-in fixture pairs.                         |
| `eval_model_on_split`   | Evaluate an ONNX tiny-AI model on a parquet feature split.                         |
| `compare_models`        | Rank ONNX models on the same split by PLCC.                                        |
| Tool            | Description                                                  |
| --------------- | ------------------------------------------------------------ |
| `vmaf_score`    | Score a (ref, dis) YUV pair. Returns the full JSON report.   |
| `list_models`   | Enumerate models under `model/` (`.json`, `.pkl`, `.onnx`).  |
| `list_backends` | Report which backends (`cpu`/`cuda`/`sycl`/`hip`) are live.  |
| `run_benchmark` | Run `testdata/bench_all.sh` on a pair.                       |
| `eval_model_on_split` | Evaluate an ONNX tiny-AI model on a parquet feature split. |
| `compare_models` | Rank ONNX models on the same split by PLCC. |
| `describe_worst_frames` | Extract the lowest-VMAF frames and describe visible artefacts with local VLM extras. |

## Install

```bash
cd mcp-server/vmaf-mcp
pip install -e .
```

Requires a built `libvmaf` binary at `build/tools/vmaf` (override via
`VMAF_BIN=/abs/path/to/vmaf`).

## Run

```bash
# Stdio transport (default for Claude Desktop, Cursor, etc.)
vmaf-mcp
```

## Path allowlisting

For safety, the server only reads files under `testdata/`,
`python/test/resource/`, and `model/`. Extend via colon-separated
`VMAF_MCP_ALLOW`:

```bash
VMAF_MCP_ALLOW=/data/my-corpus:/mnt/yuv vmaf-mcp
```

## Claude Desktop config

```json
{
  "mcpServers": {
    "vmaf": {
      "command": "vmaf-mcp",
      "env": {
        "VMAF_BIN": "/home/you/dev/vmaf/build/tools/vmaf",
        "VMAF_MCP_ALLOW": "/data/yuv-corpus"
      }
    }
  }
}
```
