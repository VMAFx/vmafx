# ADR-0774: MCP server audit — path rename, subsample drop, schema drift, dead code

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: mcp, server, audit, fork-local

## Context

A static audit of `mcp-server/vmaf-mcp/src/vmaf_mcp/server.py` against
five concern categories (isError correctness, silent error swallowing,
resource leaks, schema/surface drift, CLI/MCP parity) identified seven
findings.  Two are high-severity: the `libvmaf/` → `core/` rename
(ADR-0700) left stale hardcoded paths that cause `list_extractors` to
return an empty list silently, and the `subsample` parameter on
`vmaf_score_encoded` is accepted by the schema but never forwarded to
the vmaf CLI (`--subsample` flag), so callers who pass `subsample=N>1`
silently score every frame.

Full findings are in
[docs/research/mcp-server-audit-2026-05-29.md](../research/mcp-server-audit-2026-05-29.md).

## Decision

Fix all seven findings in a single PR:

1. **Finding 4 (High)** — Replace `libvmaf/src/feature` with
   `core/src/feature` in `_list_extractors`, and drop the dead
   `libvmaf/build/tools/vmaf` candidate from `_vmaf_binary`.
2. **Finding 5 (High)** — Wire `subsample` through `ScoreRequest` and
   append `--subsample N` to the vmaf argv when `N > 1`.
3. **Finding 7 (Medium)** — Narrow `bitdepth` enum in `vmaf_score` and
   `describe_worst_frames` tool schemas from `[8,10,12,16]` to
   `[8,10,12]`.
4. **Finding 2 (Medium)** — Remove the dead first `_run_benchmark`
   definition (lines 722–799).
5. **Finding 3 (Low)** — Replace the bare `except Exception: pass` in
   `_send_progress` with a DEBUG-level log.
6. **Finding 6 (Low)** — Log VLM load failures at WARNING level in
   `_load_vlm`.
7. **Finding 8 (Low)** — Add an early return with an empty list (not an
   exception) when `feature_dir` does not exist, and log a WARNING.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Fix only high-severity findings | Smaller diff | Leaves known debt; low-severity findings will recur | Address all seven while context is fresh |
| Introduce `--frame_step` alias | Upstream uses `--subsample`; alias avoids breakage | Adds complexity; no existing users to break | `--subsample` is the canonical flag per `cli_parse.c` |

## Consequences

- **Positive**: `list_extractors` returns correct results after the ADR-0700
  rename; `vmaf_score_encoded` respects the `subsample` parameter; schema
  no longer advertises unsupported `bitdepth=16`.
- **Negative**: None; all changes are bug-fixes with no public-API breakage.
- **Neutral / follow-ups**: Add regression tests for `list_extractors`
  path resolution and `subsample` forwarding.

## References

- ADR-0700: `libvmaf/` → `core/` rename.
- Research digest: `docs/research/mcp-server-audit-2026-05-29.md`.
- Memory note: `project_mcp_iserror_must_be_true.md`.
- `core/tools/cli_parse.c` line 224 (`--subsample` flag definition).
