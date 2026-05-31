<!-- markdownlint-disable MD013 MD060 -->
# ADR-0638: MCP P1 surface — vmaf-tune integration, list_extractors, describe_model, progress notifications

- **Status**: Accepted
- **Date**: 2026-05-19
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: `mcp`, `vmaf-tune`, `api`, `docs`

## Context

The MCP server shipped seven tools in the P0 surface (ADR-0172 / ADR-0495 / ADR-0513).
The P1 scope adds five new tools and cross-cutting progress notifications:

1. **`list_extractors`** — enumerate all `VmafFeatureExtractor` implementations
   in the C source tree without requiring a running binary.  Fills a gap: agents
   had no programmatic way to discover what feature names are available.

2. **`describe_model`** — return metadata (type, feature list, size) for a named
   VMAF model.  The prior agent (a770012d) identified a real `Path.stem` bug: for
   a model named `vmaf_v0.6.1`, `Path("vmaf_v0.6.1").stem` returns `"vmaf_v0.6"`
   (Python strips `".1"` as if it were a file extension).  The fix matches model
   files by stripping only known extensions (`.json`, `.pkl`, `.onnx`) via
   `os.path.splitext`, not by using `Path.stem`.

3. **`run_compare`** — wrap `vmaf-tune compare`, surfacing codec-comparison reports
   over MCP.  Fills the gap between the existing `run_benchmark` (fixed harness)
   and the ability to run arbitrary codec comparisons.

4. **`run_ladder`** — wrap `vmaf-tune ladder`, building per-title bitrate ladders.

5. **`run_tune_per_shot`** — wrap `vmaf-tune tune-per-shot`, providing per-shot
   CRF recommendations over MCP.

6. **Progress notifications** (cross-cutting) — `run_benchmark`, `run_compare`,
   `run_ladder`, and `run_tune_per_shot` all emit `notifications/progress` when the
   client supplies a `_meta.progressToken` (MCP spec §notifications/progress).
   These tools run 30 s–3 min; without progress the client has no feedback.

The vmaf-tune tools wrap the `vmaf-tune` CLI binary rather than importing the
Python package directly.  The MCP server runs in a separate process from the
container's Python environment; editable-install state of vmaf-tune may differ.
Wrapping the binary is the stable integration seam.

## Decision

We will add five new tools to `mcp-server/vmaf-mcp/src/vmaf_mcp/server.py`:
`list_extractors`, `describe_model`, `run_compare`, `run_ladder`,
`run_tune_per_shot`.  We will retrofit progress notifications onto
`run_benchmark` and all three new `run_*` tools using the mcp library's
`server.request_context.session.send_progress_notification()` API.
The `_vmaftune_binary()` resolver follows the same priority-list pattern as the
existing `_vmaf_binary()` resolver (env var → PATH → in-tree fallback).
The `_strip_model_ext()` helper replaces any `Path.stem` usage in model
name matching, fixing the `.1` truncation bug permanently.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Import vmaftune Python package directly | No subprocess overhead; structured return values | MCP server and vmaf-tune may be in different venvs; circular editable-install coupling | Subprocess is the stable seam |
| Parse C headers for extractor names | Matches the public API surface | Headers don't always carry the name string; only the `.c` struct definitions do | C source parse is authoritative |
| Use `Path.stem` with a pre-strip | Shorter code | Still wrong for `vmaf_v0.6.1` if caller passes a bare name | `os.path.splitext` with extension whitelist is unambiguous |
| Finer-grained progress (per encode) | Better UX for long runs | vmaf-tune gives no hook for per-encode progress from subprocess | Start + completion notifications are what the subprocess model allows |

## Consequences

- **Positive**: MCP clients can now enumerate extractors, inspect model metadata,
  and drive vmaf-tune workflows without shell access.  Progress notifications
  unblock use of `run_*` tools from clients with timeout guards.
- **Negative**: The vmaf-tune tools require `vmaf-tune` to be installed; the error
  message is clear but the tools are silently unavailable in a CPU-only MCP
  install without vmaf-tune.
- **Neutral / follow-ups**: `list_extractors` parses the source tree, so it
  returns all registered extractors regardless of which backends were compiled in.
  A future tool could filter by the running binary's compiled-in backends.
  The `run_*` tools accept `--format json` and parse stdout; non-JSON stdout
  (e.g. HLS manifest) is returned as a raw string under `{"manifest": "..."}`.

## References

- Prior agent session: a770012d89c6aef9d — identified Path.stem bug mid-debug.
- MCP spec §notifications/progress: <https://modelcontextprotocol.io/docs/specification/server/notifications/progress>
- req: "Second attempt at MCP P1 surface expansion. Prior agent died mid-debug after finding a real Path.stem bug."
- ADR-0172 (describe_worst_frames / T6-6)
- ADR-0495 (backend-probe allowlist)
- ADR-0513 (run_benchmark / bench_all.sh)
- ADR-0638 stub reserved by `scripts/adr/next-free.sh --claim mcp-p1-vmaftune-extractors-models-progress`
