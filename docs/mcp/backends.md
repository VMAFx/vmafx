# MCP backend discovery and the default allowlist

This page documents the two MCP-server defaults that the
[ADR-0511](../adr/0511-mcp-backend-probe-allowlist-and-ladder-backend.md)
cluster fixed: how `list_backends` discovers which GPU runtimes the
local `vmaf` binary supports, and which filesystem paths the server
accepts by default for input YUVs.

## How `list_backends` discovers compiled-in backends

The `list_backends` tool reports which scoring backends the local
`vmaf` binary was compiled with. The result is a JSON object with
boolean values:

```json
{
  "cpu":    true,
  "cuda":   true,
  "sycl":   false,
  "vulkan": true,
  "hip":    false,
  "metal":  false
}
```

`cpu` is always `true` — it has no driver dependency. Every GPU
backend is `true` iff the local `vmaf` binary advertises a
corresponding `--no_<backend>` disable flag in its `--help` output.

### Why `--help`, not `--version`

Historically the probe grepped the output of `vmaf --version` for the
substrings `"cuda"`, `"sycl"`, `"vulkan"`, ... The fork's `vmaf`
banner does not list compiled-in GPU backends there, so on hosts
where the kernel + driver + binary all supported (e.g.) CUDA — and
`vmaf --backend cuda` produced valid scores — `list_backends`
incorrectly returned `cuda=false`. The
[`vmaf-dev-mcp`](../development/dev-mcp.md) container surfaced this:
`docker exec vmaf-dev-mcp vmaf --backend cuda -r src01_hrc00.yuv ...`
produced a working 76.667 score while the MCP layer claimed CUDA was
unavailable. Any downstream MCP client that orchestrates a cross-
backend run picked the wrong arm and silently never ran CUDA.

The `--help` table is a stable contract: argparse-side
`--no_<backend>` flags are added at the same time as backend support,
so flag presence is sufficient and necessary for compile-in. ADR-0509
locks the probe to that table.

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

1. `<repo_root>/testdata` — fork-added YUV fixtures and benchmark
   harnesses.
2. `<repo_root>/python/test/resource` — Netflix golden fixture tree
   (includes the `yuv/`, `model/`, and `feature/` sub-directories).
3. `<repo_root>/model` — shipped VMAF model JSON / PKL / ONNX files.
4. `/workspace/python/test/resource` — the absolute container path
   for the Netflix golden fixture tree. The
   [`vmaf-dev-mcp`](../development/dev-mcp.md) container bind-mounts
   the repo root at `/workspace/`, so the golden YUVs live at
   `/workspace/python/test/resource/yuv/` inside the container. The
   entry is intentionally additive — the host-relative
   `<repo_root>/python/test/resource` still works for non-container
   invocations (e.g. `make test`).

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
