<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1117: MCP `vmaf_score` tiny-AI / feature / CTC parameter coverage

- **Status**: Accepted
- **Date**: 2026-06-14
- **Deciders**: Lusoris
- **Tags**: mcp, ai, docs, agents, fork-local

## Context

The fork's MCP servers (`cmd/vmafx-mcp` Go + `mcp-server/vmaf-mcp` Python)
shell out to the `vmaf` CLI but exposed only a small slice of its flags. The
RC-readiness MCP-coverage audit (Workstream C in
`.workingdir2/rc/RC_SCOPE_LOCKED.md`) found the fork's single largest
capability gap: the Tiny-AI / DNN scoring surface — `--tiny-model`,
`--tiny-device` / `--dnn-ep`, `--tiny-threads`, `--tiny-fp16`,
`--tiny-model-verify`, `--tiny-codec`, `--tiny-preset`, `--tiny-crf`,
`--tiny-resize`, and `--no-reference` (NR mode) — was **0 % reachable** over
MCP. Feature selection (`--feature`), the CTC presets (`--aom_ctc` /
`--nflx_ctc`), and several score-completeness flags (`--threads`,
`--frame_cnt`, `--frame_skip_ref`/`--frame_skip_dist`, `--no_prediction`) were
likewise unreachable.

A hard invariant constrains the fix: the Go and Python servers must stay
byte-compatible — identical tool names, JSON schemas, param names, enums, and
defaults — so IDE MCP clients configured against either server keep working
(see `cmd/vmafx-mcp/AGENTS.md` §1). The flag names and accepted values are
defined by `core/tools/cli_parse.c`, which is the source of truth the handlers
map onto.

## Decision

We add the tiny-AI/DNN, feature-selection, CTC-preset, and frame-range
parameters as **optional** properties on the existing `vmaf_score` and
`vmaf_score_encoded` tools (rather than new tools), in both servers, with a
byte-identical schema. Each parameter is forwarded to the `vmaf` CLI **only
when supplied**, so omitting them reproduces the prior behaviour exactly
(backward-compatible). A shared schema generator
(`scoringExtraProperties()` in Go, `_scoring_extra_properties()` in Python) and
a shared argv builder (`scoreExtras` / `ScoreExtras`) keep the two tools and
the two servers in lock-step. `--no-reference` makes the `ref` argument
optional and is gated on `tiny_model` being present, mirroring the CLI's own
`cli_parse.c` check.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Optional params on the existing two score tools (chosen) | No new tool surface; one schema for IDE clients; backward-compatible; mirrors the CLI 1:1 | `vmaf_score` schema grows to ~27 properties | Smallest blast radius; existing callers untouched; the params are intrinsically *parameters of a score*, not separate operations |
| New dedicated `vmaf_score_tiny` / `vmaf_score_nr` tools | Keeps each tool's schema small | Doubles the tool count; duplicates the geometry/path/backend plumbing; an FR+tiny call would need two tools | Splits one operation across tools and inflates the surface the byte-compat invariant must police |
| Expose a raw `extra_args: string[]` pass-through | Trivially covers every flag, now and future | No schema validation, no enums, no discoverability; an injection/foot-gun surface; un-typed for IDE clients | Defeats the point of a typed MCP schema and the path-allowlist security posture |

## Consequences

- **Positive**: the fork's defining Tiny-AI/DNN surface is reachable over MCP
  for the first time; CTC presets and feature selection are one call away; NR
  (no-reference) scoring works end-to-end through both servers. The Go and
  Python schemas are verified byte-identical for both score tools.
- **Negative**: the two score-tool schemas are larger; future flag additions
  must be made in two places (Go + Python) and kept byte-identical — the
  shared generator localises this but does not eliminate it.
- **Neutral / follow-ups**: the cgo direct path (ADR-0931) does not yet
  understand the new flags, so any request that sets one routes through the
  subprocess path (the existing fallback contract). `--csv` / `--sub` output
  formats are deliberately **not** exposed on the score tools because the
  handlers parse the JSON output file; exposing them would break parsing. The
  pre-existing `precision` doc/code default drift (`docs/mcp/tools.md` said
  `"17"`, code passes `"legacy"`) was corrected in passing. Stale tool-count
  comments (`main.go` "16 tools", `server.py` "ten tools") were corrected to
  the real count of 15.

## References

- `.workingdir2/rc/RC_SCOPE_LOCKED.md` — "Workstream C — MCP coverage gap list"
  (gap #1 Tiny-AI/DNN 0 % reachable, RC-PRIORITY 1; gap #2 feature/CTC; gap #4
  score-param completeness; gap #7 stale "16 tools" comment).
- `core/tools/cli_parse.c` — authoritative flag names + accepted values.
- `cmd/vmafx-mcp/AGENTS.md` §1 — Go/Python tool name + schema parity invariant.
- [ADR-0009](0009-mcp-server-tool-surface.md) — original MCP tool surface.
- [ADR-0634](0634-mcp-p0-iserror-and-probe-version-encoded.md) — MCP P0
  (`isError`, probe, `vmaf_score_encoded`).
- [ADR-0638](0638-mcp-p1-vmaftune-extractors-models-progress.md) — MCP P1 tool
  expansion (`list_extractors`, `describe_model`, vmaf-tune).
- [ADR-0931](0931-mcp-cgo-direct-replace-subprocess.md) — cgo direct-path
  fallback contract.
- [ADR-0119](0119-cli-precision-default-revert.md) — `--precision` legacy default.
- Source: `req` — user directive to "close the biggest 100 % tooling capability
  gaps — the Tiny-AI/DNN scoring surface (0 % reachable via MCP) + feature
  selection + CTC presets — by adding params to the existing `vmaf_score` and
  `vmaf_score_encoded` MCP tools", keeping the Go and Python servers
  byte-compatible.
