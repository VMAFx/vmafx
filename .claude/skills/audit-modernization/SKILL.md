---
name: audit-modernization
description: Replay the project modernization audit on the current tree and produce /tmp/modernization-audit-YYYY-MM-DD.md. Wraps scripts/dev/project_modernization_audit.py with sensible defaults and timestamped output.
---

<!-- markdownlint-disable MD013 -->

# /audit-modernization

Runs `scripts/dev/project_modernization_audit.py` over the curated scan roots
(`.github/workflows`, `ai/`, `docs/{ai,api,backends,development,mcp,metrics,usage}/`,
`core/src/`, `core/tools/`, `mcp-server/`, `scripts/`, `tools/`, plus
`.workingdir2/` state files) and writes a dated Markdown report to
`/tmp/modernization-audit-YYYY-MM-DD.md`. The audit is **read-only** — it does
not edit BACKLOG.md, open PRs, or run external tools.

Use this skill at the start of a planning session, before dispatching agents,
or after a large merge train clears, to get an objective list of what's still
stubbed / scaffolded / deferred. Pairs with `BACKLOG.md` (the user's prioritized
intent) — the audit shows what the *code* says is open; BACKLOG.md shows what
the *user* says should land first. Reconcile the two before starting work.

## When to use

- "What's left to do?" / "What should I work on next?" — run the audit, then
  cross-reference with `.workingdir2/BACKLOG.md`.
- After a multi-PR merge train — confirm the closed items genuinely cleared
  out of the code (no orphan stubs left behind).
- Before opening a modernization meta-issue — the audit output is the seed
  list.
- NOT for finding bugs — use `make lint` + `/lint-all` for that.
- NOT for tracking ADR coverage — see `docs/adr/README.md` and the
  ADR-0108 compliance audits.

## Invocation

```text
/audit-modernization [--include-archives] [--max-findings=N] [--out=PATH]
```

All flags optional:

- `--include-archives` — also scan paths containing an `archive/` component
  (off by default — archived code is intentionally frozen).
- `--max-findings=N` — cap the rendered Markdown rows (default 30; pass a
  larger number for a full sweep, smaller for an executive summary).
- `--out=PATH` — override the default output path
  `/tmp/modernization-audit-YYYY-MM-DD.md`.

## What it scans

Markers ranked into priority tiers by `project_modernization_audit.py`:

- **Stubs / scaffolds** — `TODO(stub)`, `// stub`, `NotImplementedError`,
  `panic("TODO`, `raise NotImplementedError`, scaffold-only files.
- **Deferred work** — `// DEFER`, `# DEFER`, `// FIXME(deferred)`,
  `TODO(<owner>): defer`.
- **Placeholders** — `placeholder`, `dummy`, `stub-only`, `not-yet-impl`,
  `coming soon`, `TBD`.
- **Implementation TODOs** — generic `TODO:` markers (lowest priority — too
  noisy to action without context).
- **State-file rows** — open T-* tier rows in `.workingdir2/BACKLOG.md`,
  `.workingdir2/OPEN.md`, `.workingdir2/PLAN.md`.

## Output

A Markdown file at `/tmp/modernization-audit-YYYY-MM-DD.md` containing:

1. **Summary table** — marker counts per scan root + grand total.
2. **Top findings** — `--max-findings` rows, sorted by priority tier, with
   file:line, marker text, and one-line context.
3. **State-file digest** — open T-* rows from `.workingdir2/`.
4. **Per-file index** — counts per file for files exceeding
   `--max-per-file` (default 5) markers.

The script also accepts `--out-json=PATH` for a machine-readable companion;
the skill does not emit JSON by default but downstream automation can.

## Workflow

1. `cd $(git rev-parse --show-toplevel)`.
2. Compute date stamp: `date +%Y-%m-%d`.
3. Run the audit script with the curated defaults baked into the script (no
   `--scan-root` overrides), redirecting to the timestamped output path.
4. Print the output path + a 5-line summary (top tier counts) to stdout so
   the caller knows what to read.
5. Suggest the next action (cross-reference with `.workingdir2/BACKLOG.md`
   and `docs/state.md`).

## Guardrails

- **Never** edits any tracked file. Output lands under `/tmp/` only.
- **Never** runs git, network, or build commands — pure local scan.
- **Never** auto-dispatches agents based on findings — the audit is a
  read-only seed list; the human decides what becomes a PR.
- If the script is missing or fails, the skill reports the error and exits
  non-zero (do not silently fall back to a degraded scan).

## References

- [`scripts/dev/project_modernization_audit.py`](../../../scripts/dev/project_modernization_audit.py) — implementation
- [`scripts/dev/test_project_modernization_audit.py`](../../../scripts/dev/test_project_modernization_audit.py) — pytest coverage
- [`.workingdir2/BACKLOG.md`](../../../.workingdir2/BACKLOG.md) (gitignored) — prioritized intent
- [`docs/state.md`](../../../docs/state.md) — bug-status tracker (ADR-0165)
- Global rule: "Read AND update local state files" (user CLAUDE.md)
