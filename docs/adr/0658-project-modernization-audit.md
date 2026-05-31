<!-- markdownlint-disable MD060 -->
# ADR-0658: Project modernization audit

- **Status**: Accepted
- **Date**: 2026-05-20
- **Deciders**: Lusoris maintainers
- **Tags**: ai, tooling, docs, backlog

## Context

The fork now has many active workstreams: refreshed AI tables, HDR/MOS training,
external scorer sidecars, vmaf-tune profile reports, backend parity, and local
gitignored planning files. Manual searches for `(stub)`, `scaffold`,
`deferred`, and `limitations` still find useful work, but the results are easy
to mix with historical ADR prose, archived scratch, or genuinely blocked model
artifact debt.

We need a repeatable operator tool that turns those signals into a ranked
action queue without becoming a CI gate or editing `.workingdir2` on its own.

## Decision

We will add `scripts/dev/project_modernization_audit.py`, a read-only scanner
for curated source/doc roots, model registry smoke rows, AI script-family
clusters, and local state files. It emits JSON and Markdown reports, marks
blocked/deferred rows separately from actionable findings, and keeps
`.workingdir2/OPEN.md` / `.workingdir2/BACKLOG.md` as the editorial state of
record.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Continue ad hoc `rg` sweeps | No new code; flexible | Easy to repeat stale hits, miss local state rows, and lose findings across sessions | The user explicitly wants the backlog preserved and worked continuously, not rediscovered manually every time |
| Make the audit a required CI gate | Prevents new stubs from entering unnoticed | The scanner is text-based and would fail on intentional disabled-build contracts, ADR history, and model-artifact blockers | Too noisy for CI; the right contract is advisory queue shaping |
| Let the tool rewrite `.workingdir2` | Keeps state files automatically current | Machine edits to local planning files risk deleting context that needs human judgement | Existing project rules keep local state updates editorial; the tool stays read-only |

## Consequences

- **Positive**: operators get a ranked, reproducible report of actionable
  modernization work, including code markers, stale docs, smoke-model rows, and
  large one-off AI script families.
- **Negative**: the scanner is name/text based; a human still has to decide
  whether a finding is real debt, intentional contract, or historical prose.
- **Neutral / follow-ups**: use the report to refresh `.workingdir2/OPEN.md`
  and choose PRs, but do not add the audit to mandatory CI without a separate
  false-positive reduction pass.

## References

- Research: [Research-0658](../research/0658-project-modernization-audit.md)
- Related: [ADR-0650](0650-signal-mix-audit.md)
- Source: req: "well go on i guess we have enough backlog..."
