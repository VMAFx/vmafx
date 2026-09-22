---
name: audit-modernization
description: Replay the project modernization audit on the current tree and produce /tmp/modernization-audit-YYYY-MM-DD.md. Wraps scripts/dev/project_modernization_audit.py with sensible defaults and timestamped output.
---
<!-- markdownlint-disable MD013 -->

# /audit-modernization

Runs `scripts/dev/project_modernization_audit.py` over curated scan roots
(`.github/workflows`, `ai/`,
`docs/{ai,api,backends,development,mcp,metrics,usage}/`, `core/src/`,
`core/tools/`, `mcp-server/`, `scripts/`, `tools/`, `.workingdir/` state
files) -> dated Markdown report at `/tmp/modernization-audit-YYYY-MM-DD.md`.
Audit = **read-only**: does not edit BACKLOG.md, open PRs, run external tools.

Use at start of planning session, before dispatching agents, or after large
merge train clears -> objective list of stubbed / scaffolded / deferred code.
Pairs with `BACKLOG.md` (user prioritized intent): audit = what code says is
open; BACKLOG.md = what user says lands first. Reconcile both before work.

## When to use

- "What's left to do?" / "What should I work on next?" -> run audit,
  cross-reference with `.workingdir/BACKLOG.md`.
- After multi-PR merge train -> confirm closed items cleared from code
  (no orphan stubs left).
- Before opening modernization meta-issue -> audit output = seed list.
- NOT for finding bugs -> use `make lint` + `/lint-all`.
- NOT for tracking ADR coverage -> see `docs/adr/README.md` and
  ADR-0108 compliance audits.

## Invocation

```text
/audit-modernization [--include-archives] [--max-findings=N] [--out=PATH]
```

Flags (all optional):

- `--include-archives` -> scan paths with `archive/` component (default = off;
  archived code intentionally frozen).
- `--max-findings=N` -> cap rendered Markdown rows (default = 30; larger
  = full sweep, smaller = executive summary).
- `--out=PATH` -> override default output path
  `/tmp/modernization-audit-YYYY-MM-DD.md`.

## What it scans

Markers ranked into priority tiers by `project_modernization_audit.py`:

- **Stubs / scaffolds** -> `TODO(stub)`, `// stub`, `NotImplementedError`,
  `panic("TODO`, `raise NotImplementedError`, scaffold-only files.
- **Deferred work** -> `// DEFER`, `# DEFER`, `// FIXME(deferred)`,
  `TODO(<owner>): defer`.
- **Placeholders** -> `placeholder`, `dummy`, `stub-only`, `not-yet-impl`,
  `coming soon`, `TBD`.
- **Implementation TODOs** -> generic `TODO:` markers (lowest priority;
  too noisy to action without context).
- **State-file rows** -> open T-* tier rows in `.workingdir/BACKLOG.md`,
  `.workingdir/OPEN.md`, `.workingdir/PLAN.md`.

## Output

Markdown file at `/tmp/modernization-audit-YYYY-MM-DD.md` contains:

1. **Summary table** -> marker counts per scan root + grand total.
2. **Top findings** -> `--max-findings` rows, sorted by priority tier;
   file:line, marker text, one-line context.
3. **State-file digest** -> open T-* rows from `.workingdir/`.
4. **Per-file index** -> counts per file for files exceeding
   `--max-per-file` (default = 5) markers.

Script accepts `--out-json=PATH` for machine-readable companion. Skill does not
emit JSON by default; downstream automation can.

## Workflow

1. `cd $(git rev-parse --show-toplevel)`.
2. Compute date stamp: `date +%Y-%m-%d`.
3. Run audit script with curated defaults baked into script (no
   `--scan-root` overrides) -> redirect to timestamped output path.
4. Print output path + 5-line summary (top tier counts) to stdout.
5. Suggest next action (cross-reference with `.workingdir/BACKLOG.md`
   and `docs/state.md`).

## Guardrails

- **Never** edits tracked file. Output lands under `/tmp/` only.
- **Never** runs git, network, or build commands: pure local scan.
- **Never** auto-dispatches agents from findings: audit = read-only seed
  list; human decides what becomes PR.
- If script missing or fails: report error, exit non-zero (do not silently fall
  back to degraded scan).

## References

- [`scripts/dev/project_modernization_audit.py`](../../../scripts/dev/project_modernization_audit.py)
  = implementation
- [`scripts/dev/test_project_modernization_audit.py`](../../../scripts/dev/test_project_modernization_audit.py)
  = pytest coverage
- `.workingdir/BACKLOG.md` (gitignored)
  = prioritized intent
- [`docs/state.md`](../../../docs/state.md) = bug-status tracker (ADR-0165)
- Global rule: "Read AND update local state files" (user CLAUDE.md)
