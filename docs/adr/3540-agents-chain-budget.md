# ADR-3540: Cap every `AGENTS.md` load path at 32 KiB

- **Status**: Accepted
- **Date**: 2026-11-12
- **Tags**: docs, agents

## Context

Codex reads one `AGENTS.md` per directory from the repository root down to the
working directory and truncates the deepest file once the byte sum passes
`project_doc_max_bytes` = 32,768 bytes (Codex `rust-v0.165.0`,
`codex-rs/core/src/agents_md.rs`). A directory's load path is the byte sum of
every tracked `AGENTS.md` from the repository root to that directory,
inclusive.

On `master` at commit `d25215367`, 26 directories exceeded the 32,768-byte cap
(issue 3191). The deepest chains were `core/src/feature/arm64` (63,793 bytes),
`core/src/sycl` (57,946) and `core/src/cuda` (57,162) inside `core/`, and
`scripts/release` (55,484), `scripts/lib` (45,183), `scripts/dev` (42,830),
`scripts/ci/lint-stubs` (39,873) and `scripts/ci` (39,027) outside `core/`.
Unmigrated shared parents (`scripts/AGENTS.md` at 25,444 bytes and
`cmd/AGENTS.md` at 4,904 bytes, atop root `AGENTS.md` at 11,158 bytes) consumed
most of the budget before a subdirectory's own invariants were read, so even
directories that had already adopted [ADR-1454](1454-agents-index-and-topic-pages.md)
(`scripts/ci` at 2,425 bytes) exceeded 32,768 bytes in aggregate.

Separately, `.cursor/rules/vmafx-workspace.mdc` carried three workspace rules
outside `AGENTS.md` (`.vscode/` defaults and `clangd` as the single C/C++
language server, keeping every agent integration enabled, and running the
smallest checked-in task while keeping build output out of tracked files). The
next `praetorctl compile-context --verify` release refuses hand-maintained
agent instruction files outside `AGENTS.md` (issue 3346).

## Decision

Cap every tracked `AGENTS.md` load path from the repository root to a directory
at 32,768 bytes, enforced by `CHAIN_MAX_BYTES` in
`scripts/docs/agents_index.py` (`make docs-fragments-check`). Maintain a
30,720-byte (30 KiB) working target when shrinking or splitting files so later
invariant additions retain 2,048 bytes of headroom below `CHAIN_MAX_BYTES`.

Shrink shared parent directories first into short `_index.md` heads that route
through rendered topic tables and areas as defined in
[ADR-1454](1454-agents-index-and-topic-pages.md). Migrate hand-written leaf
`AGENTS.md` files into `<dir>/AGENTS.d/` topic pages without rewording
(`scripts/docs/agents_migration_check.py`) when their load path still exceeds
the working target after their parents shrink.

Where a rendered index is mostly its topic table, group its pages into areas
(`AGENTS.d/_area-<slug>.md` plus `area:` on each page) so the index lists
areas and each area gets its own sub-index; `core/AGENTS.md`,
`core/src/AGENTS.md`, `core/src/feature/AGENTS.md` and
`core/src/feature/sycl/AGENTS.md` take this route. Shorten the generated
banner and usage paragraph every rendered index repeats to at most 350 bytes.
`--write` runs the same load-path check as `--check` on the rendered bytes, so
a render that crosses the cap exits 65 and names every file on the path.

Move the three workspace rules from `.cursor/rules/vmafx-workspace.mdc` into
root `AGENTS.md` (`## 11. Workspace`) and remove
`.cursor/rules/vmafx-workspace.mdc` so `praetorctl compile-context` generates
the Cursor projection from the single canonical root file.

## Alternatives considered

| Option | Benefit | Cost | Outcome |
| --- | --- | --- | --- |
| Raise Codex `project_doc_max_bytes` in a checked-in config | Avoids splitting existing `AGENTS.md` files | Only helps one client and one configuration path; Codex, Gemini, Antigravity, opencode, Claude Code and editors each enforce separate instruction budgets | Rejected |
| One flat index at the repository root | Eliminates multi-directory chain sums | Loses directory-scoped routing and pushes root `AGENTS.md` past the cap on every task | Rejected |
| Trim invariant text in place | Keeps single-file layout without `AGENTS.d/` | Drops binding invariants; ADR-1454 already measured five register passes that could not shrink dense files | Rejected |
| Enforce `CHAIN_MAX_BYTES` = 32,768 (30,720 target) with parent-first ADR-1454 splits and root workspace rules | Keeps every load path within cap across clients with zero lost invariants and single-source context compilation | Adds `AGENTS.d/` directories for oversized parents and leaves | Chosen |

## Consequences

- Every `AGENTS.md` load path loads without truncation under the 32,768-byte
  cap: all 76 paths measure at most 29,958 bytes (`ai/sidecar`), below the
  30,720-byte working target.
- `scripts/docs/agents_index.py --check` (`make docs-fragments-check`) fails
  closed when an edit to root `AGENTS.md`, an intermediate parent or a leaf
  pushes any load path past `CHAIN_MAX_BYTES`.
- Shrinking `scripts/AGENTS.md` (25,444 to 2,501 bytes) and `cmd/AGENTS.md`
  (4,904 to 1,435 bytes) brings smaller children (`scripts/lib`, `scripts/dev`,
  `scripts/ci`, `scripts/ci/lint-stubs`, `cmd/vmafx-node`) within budget
  without modifying their leaf files, while larger leaves (`scripts/release`,
  `cmd/vmafx-controller`, `cmd/vmafx-tune`, `cmd/vmafx-mcp` and the `core/`
  leaves) become ADR-1454 topic or area indexes verified lossless by
  `scripts/docs/agents_migration_check.py`.
- Root `AGENTS.md` holds all workspace and agent-integration rules (`## 11.
  Workspace`), and `.cursor/rules/vmafx-workspace.mdc` is deleted so
  `praetorctl compile-context --verify` has a single canonical source.

## References

- [ADR-0106: Decision records](0106-adr-maintenance-rule.md).
- [ADR-1454: Generated subtree `AGENTS.md` index](1454-agents-index-and-topic-pages.md).
- Contributor guide:
  [Agents index and topic pages](../development/agents-index.md#load-path-budget).
- Codex `rust-v0.165.0`, `codex-rs/core/src/agents_md.rs`:
  `project_doc_max_bytes` = 32,768, measured against commit `d25215367`.
- Issues 3191 (`AGENTS.md` load-path cap) and 3346
  (`.cursor/rules/vmafx-workspace.mdc` outside `AGENTS.md`).
