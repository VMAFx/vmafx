<!-- markdownlint-disable MD013 MD060 -->
# ADR-0634: MCP P0 fixes — isError spec bug, probe_backend, vmaf_version, vmaf_score_encoded

- **Status**: Accepted
- **Date**: 2026-05-19
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `mcp`, `bugfix`, `spec-correctness`, `fork-local`

## Context

The MCP capability audit of 2026-05-19 (`docs/research/mcp-capability-audit-2026-05-19.md`)
identified four P0 (operator-blocking) findings in the Python standalone MCP server
(`mcp-server/vmaf-mcp/src/vmaf_mcp/server.py`):

**C-P0-1 / E-1 — `isError=False` spec-correctness bug** (`server.py:983-984`). The
`_call_tool` handler caught all exceptions and returned `[TextContent({"error": ...})]`
without setting `isError=True` on the `CallToolResult`. Conformant MCP clients
(`mcp/client/session.py:L394`) branch on `result.isError` — with the old code they
silently treated every tool error as a success, then failed downstream trying to parse
`{"error": "..."}` as a VMAF score result. The MCP 2024-11 spec designates `isError=True`
as the canonical signal for tool-level errors.

**C-P0-1 (tool gap) — No `probe_backend` tool**. `list_backends` reported compile-in
status only. On the dev-mcp container, backends have historically been compiled in but
non-functional at runtime (SYCL NEO version mismatch, HIP KFD ioctl failure, Vulkan ICD
ordering). An operator asking "is CUDA healthy right now?" got `true` from `list_backends`
even when the driver was absent.

**C-P0-3 — No `vmaf_version` tool**. An operator debugging a score regression had no
way to confirm which fork version was running or which backends were compiled in without
calling multiple tools and correlating out of band.

**C-P0-2 — `vmaf_score` accepted only raw YUV**. Real operator workflows use MP4/MKV/Y4M
files. No MCP tool decoded encoded video before scoring. Operators had to pre-decode
manually or use the CLI directly.

Additionally, two documentation correctness issues from the audit:

- `README.md:L12` listed only 4 backends (omitting vulkan, metal).
- `tools.md:L170` described the `--version`-grep probe, which was replaced with the
  `--help`-flag probe in ADR-0511.

## Decision

Four fixes shipped in one PR:

1. **`isError` fix**: remove the `try/except` catch in `_call_tool` and let exceptions
   propagate. The `mcp` library's outer handler (`_make_error_result`) then correctly
   sets `isError=True`. This is the only spec-compliant approach that works with all
   conformant MCP clients without monkey-patching the library internals.

2. **`probe_backend` tool**: new async function `_probe_backend(backend)` that writes a
   32×32 mid-grey synthetic 4:2:0 8-bit YUV pair to a `TemporaryDirectory` and runs a
   1-frame vmaf score against it. Returns `{compiled_in, runtime_healthy, latency_ms,
   score, error}`. Unhealthy backends return a success-shaped result (not a raise) because
   "backend not functional" is an expected operator query result, not a protocol error.

3. **`vmaf_version` tool**: new synchronous function `_vmaf_version()` that runs
   `vmaf --version` to extract the version string and delegates to `_probe_backends()`
   (the cached `--help` probe) for build flags. Returns `{binary_path, version,
   build_flags, error}`.

4. **`vmaf_score_encoded` tool**: new async function `_run_vmaf_score_encoded(ref, dis,
   ...)` that ffprobes the reference stream for geometry, decodes both inputs in parallel
   to a `TemporaryDirectory` via `ffmpeg -f rawvideo`, then calls `_run_vmaf_score`.
   Returns the standard `vmaf_score` payload plus `reference_encoded` and
   `distorted_encoded` fields. Geometry detection covers 8/10/12-bit YUV 4:2:0, 4:2:2,
   and 4:4:4.

Two stale documentation corrections:

- `README.md` tools table updated to all ten tools and six backends.
- `tools.md` `list_backends` description corrected from `--version` to `--help` probe;
  response body updated to show all six backends; `isError` conventions updated to
  reflect the new raise-on-error behavior.

Pre-existing test `test_call_tool_unknown_name_returns_error_json` (which asserted the
old catch-and-return behavior) updated to `test_call_tool_unknown_name_raises` asserting
the new raise behavior. `test_list_tools_returns_expected_names` updated from 7 to 10
tools.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Return `CallToolResult(content=..., isError=True)` from the catch block | Keeps the old error-shape for backward compat | Requires importing `CallToolResult` from mcp.types; still catches and re-wraps; doesn't work if the mcp library version changes its internal result type | Re-raising is simpler, idiomatic, and is the approach documented in the mcp library's own examples (`_make_error_result` is the intended mechanism) |
| Keep `probe_backend` returning a plain error TextContent on failure | Consistent with old error shape | Doesn't help: `runtime_healthy: false` is not an error — it's a valid health answer | N/A — the distinction between "probe ran, backend unhealthy" and "probe itself failed" is captured in `runtime_healthy` vs `error` field |
| Use subprocess process substitution instead of temp YUV for `vmaf_score_encoded` | No disk write | Requires shell=True or a FIFO, introduces OS portability issues | Temp file in `TemporaryDirectory` is safe, cross-platform, and already the pattern used by `_run_vmaf_score` for its output JSON |

## Consequences

- **Positive**: Conformant MCP clients (Claude Desktop, Cursor, any client that branches
  on `result.isError`) now correctly distinguish tool errors from successes. `probe_backend`
  gives operators a runtime health signal before long runs. `vmaf_version` enables build
  identification. `vmaf_score_encoded` covers the most common real-world VMAF input format.
- **Negative**: The `isError` change is a breaking behavior change for any caller that
  currently catches the `{"error": ...}` TextContent and treats `isError=False` errors as
  data. Such callers were already broken (treating errors as successes); this change makes
  the breakage visible. No such callers exist in the current test suite.
- **Neutral / follow-ups**: `vmaf_score_encoded` requires `ffmpeg` + `ffprobe` on PATH;
  this is documented. The `subsample` parameter is accepted but not yet wired to a vmaf
  `--frame_step` flag (the CLI may not support it for all backends) — the parameter is
  validated and stored but has no effect in this PR; a follow-up can wire it when needed.

## References

- `docs/research/mcp-capability-audit-2026-05-19.md` — source audit, findings C-P0-1,
  C-P0-2, C-P0-3, D-6, E-1, F-1, F-2.
- [ADR-0495](0495-mcp-probe-bug-fixes.md) — prior MCP probe bug-fix cluster.
- [ADR-0511](0511-mcp-list-backends-help-probe.md) — `--help` probe replacing `--version`
  grep (the `tools.md` description had not been updated).
- [ADR-0556](0556-python-mcp-ai-audit-2026-05-18.md) — prior MCP audit fixes.
- req: "Fix `isError=False` spec-correctness bug ... Add `probe_backend` tool ... Add
  `vmaf_version` tool ... Add `vmaf_score_encoded` tool ... in ONE DRAFT PR."
