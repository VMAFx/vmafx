# MCP backend discovery and the default allowlist

This page explains how `list_backends` discovers which GPU runtimes the
local `vmaf` binary supports, and which filesystem paths the MCP servers
accept by default for input YUVs. Both defaults were fixed by the
[ADR-0511](../adr/0511-mcp-backend-probe-allowlist-and-ladder-backend.md)
cluster.

## How `list_backends` discovers compiled-in backends

The `list_backends` tool reports which scoring backends the local
`vmaf` binary was compiled with. The result is a JSON object with
boolean values:

```json
{
  "cpu":   true,
  "cuda":  true,
  "sycl":  false,
  "hip":   false,
  "metal": false
}
```

The rule: `cpu` is always `true`, because it has no driver dependency.
Every GPU backend is `true` if and only if `vmaf --list-backends` reports it
`usable`: compiled into the binary **and** its state initialises on this host.
If the `vmaf` binary is missing, `cpu` stays `true` and every GPU flag is
`false`; no error is raised. A `vmaf` that cannot print the report (older than
ADR-1874) or prints something that is not a report is treated as CPU-only, and
the server logs why.

Both servers implement this probe: the Python server in `_probe_backends()`
and the Go server (`cmd/vmafx-mcp`) in `probeBackends()`, which reads the
report through `pkg/scorebackend`, with the same per-binary caching.

### Why `--list-backends`, not `--help` or `--version`

The `--help` text prints `--no_cuda`, `--no_sycl`, `--no_hip` and `--no_metal`
on every build, so on a CPU-only build the old `--help` probe reported every GPU
backend `true` and the backend allowlist admitted them (the run then failed
with exit 100 instead of scoring). The report asks the binary's own backend
initialisers, the calls a scoring run makes
([ADR-1874](../adr/1874-vmaf-list-backends.md)).

!!! note "History (2026-05)"
    The probe used to grep the output of `vmaf --version` for the substrings
    `"cuda"`, `"sycl"`, and so on. The fork's `vmaf` banner does not list
    compiled-in GPU backends, so on a host where the kernel, driver and binary
    all supported CUDA, `list_backends` returned `cuda=false`.

The [`vmaf-dev-mcp`](../development/dev-mcp.md) container surfaced this:
`docker exec vmaf-dev-mcp vmaf --backend cuda -r src01_hrc00.yuv ...` produced
a working 76.667 score while the MCP layer claimed CUDA was unavailable. A
client that orchestrates a cross-backend run picked the wrong arm and never ran
CUDA.

### Probe caching

`_probe_backends()` caches its result per absolute `vmaf` binary path
for the lifetime of the MCP server process so `vmaf_score` does not
fork a subprocess on every call. The cache is keyed on the resolved
path; reinstalling the binary at the same path within a single server
session continues to use the cached result (restart the server to
re-probe).

### Triggering a re-probe

The cache is process-local; the canonical way to refresh it is to
restart the MCP server. There is no MCP tool to flush the cache —
deliberately, since `list_backends` is a low-traffic read tool and
the binary on disk should not change under a running session.

## Default allowlist

Every MCP tool that accepts a filesystem path (`vmaf_score`,
`run_benchmark`, `describe_worst_frames`, ...) resolves the path via
`Path.resolve()` and rejects anything that does not live under one of
these roots:

| Root | Why it is allowed |
| --- | --- |
| `<repo_root>/testdata` | Fork-added YUV fixtures and benchmark harnesses. |
| `<repo_root>/python/test/resource` | Netflix golden fixture tree (includes `yuv/`, `model/` and `feature/`). The Python server also lists `python/test/resource/yuv` separately; it is a subset, so behaviour is the same. |
| `<repo_root>/model` | Shipped VMAF model JSON / PKL / ONNX files. |
| `/workspace/python/test/resource` | Absolute container path for the Netflix golden fixtures. The [`vmaf-dev-mcp`](../development/dev-mcp.md) container bind-mounts the repo root at `/workspace/`, so the golden YUVs live at `/workspace/python/test/resource/yuv/` there. Additive: the host-relative root still works for non-container runs (for example `make test`). |

Rejection error:

```text
ValueError: path /tmp/evil.yuv not under an allowlisted root;
set VMAF_MCP_ALLOW to extend.
```

### Extending via `VMAF_MCP_ALLOW`

Additional roots may be added at runtime via the `VMAF_MCP_ALLOW`
environment variable. The value is a colon-separated list of absolute
paths:

```bash
export VMAF_MCP_ALLOW="/data/my-corpus:/srv/yuv"
vmaf-mcp
```

Each entry is `.resolve()`-d (so symlinks are followed) and the
combined list (defaults + override) is what every tool validates
against. The override is additive — it does NOT replace the defaults.

### Path-traversal safety

Path validation is `Path.resolve()`-based: a request for
`/workspace/python/test/resource/../../etc/passwd` resolves to
`/etc/passwd` *before* the allowlist check, so symlink and `..`
traversal both fall under the same rejection. The check is
`path.is_relative_to(root)` for every configured root — strict
subset, not glob.

## See also

- [MCP tool reference](tools.md) — full schema for `vmaf_score`,
  `list_backends`, and the other MCP tools.
- [ADR-0495](../adr/0495-mcp-probe-bug-fixes.md) — the prior MCP
  probe-driven bug-fix cluster (refuse-and-echo backend selection,
  schema enum extension, benchmark error surfacing).
- [ADR-0511](../adr/0511-mcp-backend-probe-allowlist-and-ladder-backend.md)
  — this page's source ADR.
- [Container operator guide](../development/dev-mcp.md) — full
  walkthrough of the `vmaf-dev-mcp` container the default allowlist
  fix targets.

## Licensing (ADR-1250)

The MCP server is fork-authored and is licensed under EUPL-1.2, like the rest of
the fork's own code. The Go and Python implementations that were dual-licensed
`... OR MIT` lose the MIT alternative; anyone who already received those files
under MIT keeps that grant for those versions. See
[ADR-1250](../adr/1250-eupl-fork-relicense.md).
