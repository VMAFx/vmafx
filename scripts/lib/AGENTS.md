# `scripts/lib/` — agent invariants

Parent: [../AGENTS.md](../AGENTS.md).

Shared Python utilities, consumed by `scripts/ci/` and (future) by
`scripts/dev/` and `.github/workflows/*.yml`. Created by
[ADR-0355](../../docs/adr/0355-symphony-agent-dispatch-infra.md);
see [Research-0091](../../docs/research/0091-symphony-spec-review.md)
for design rationale.

## Rebase-sensitive surfaces

Nothing in this directory mirrors upstream Netflix/vmaf. Rebase risk
= "internal coupling drift", not "merge conflict".

| Module | Consumers | What couples them |
| --- | --- | --- |
| `backlog_tracker.py` | `scripts/ci/agent-eligibility-precheck.py` (direct import); future state-audit / status-reporter scripts. | The `BacklogItem` dataclass field names (`id` / `title` / `status` / `priority` / `pr_refs` / `raw_row`) and the status enum strings (OPEN / IN_FLIGHT / DONE / CLOSED / REMOVED / BLOCKED / DEFERRED). Renames are breaking changes for every importer. |
| `backlog_tracker.py` ↔ `.workingdir/BACKLOG.md` item format | Current rows are Markdown checklists with a stable backtick ID immediately after the checkbox; retired pipe-table rows remain readable. | Never derive an ID from list order, title text, or a GitHub issue reference. An unmarked checklist item, duplicate ID, unknown status marker, checkbox/status contradiction, or existing ledger with zero items must raise `BacklogFormatError`; the precheck turns that into a blocking verdict. Run `python3 -m unittest scripts.lib.test_backlog_tracker scripts.ci.tests.test_agent_eligibility_precheck` after a parser or schema edit. |
| `GitHubTracker._run` | Wraps the `gh` CLI. | Output schema (`number / title / body / headRefName / mergedAt / state`) is `gh`-version-coupled. Pin behaviour by passing `--json` field lists explicitly; never rely on default columns. |
| `safe_subprocess.py` | Python automation under `scripts/`. | Every executable is allowlisted, argv/environment/cwd are validated, captured output and runtime are bounded, and POSIX timeout, output-overflow, and caller-cancellation cleanup own the process group. `scripts/__init__.py` plus canonical `scripts.lib.safe_subprocess` imports preserve one runtime/type identity. Do not replace it with a raw `subprocess` call or an `S603` waiver. |

## Read-only invariant

`backlog_tracker.py` **never** writes to BACKLOG.md. Adding write
path = CLAUDE.md global-rule violation ("Read AND update local
state files" — *update* half is editorial, not automated). Genuine
machine-write need -> lands in separate module (e.g.
`backlog_writer.py`) with own ADR.

## Stdlib-only invariant

Module imports only from Python standard library
(`os`, `re`, `subprocess`, `dataclasses`, `datetime`, `pathlib`,
`json`, `typing`). Adding third-party dependency (PyYAML, Linear
SDK, etc.) = regression, two reasons:

1. Fork's CI lint profile runs against system Python; new wheels
   add wheels-cache surface, Sigstore-signing toil.
2. Point of this module = importable from any wrapper script
   without virtualenv setup.

New dep genuinely justified -> write ADR first.

## Process-execution invariant

Repository automation under `scripts/` launches external programs through
`safe_subprocess.run()` or `run_async()`. Every caller supplies the executable
allowlist and receives a deadline and output ceiling, using stricter explicit
values where the default is not appropriate. A missing executable, malformed
argv, timeout, or output flood fails closed.
Tests that intentionally spell `subprocess` as fixture source text are not
production launch paths. See [ADR-1270](../../docs/adr/1270-bounded-process-execution.md).
Keep `scripts/__init__.py` and the two-root mypy regression in
`scripts/git-hooks/test-pre-push-mypy.py`; removing either recreates the
`lib.safe_subprocess` / `scripts.lib.safe_subprocess` duplicate-module failure.
Keep the cancellation and fail-soft consumer cases in
`test_safe_subprocess.py` and
`scripts/ci/tests/test_agent_eligibility_precheck.py` wired to both commit and
push hooks; cancellation must terminate descendants and reap the direct child
before propagating `CancelledError`.

## Worktree-aware path resolution

`DEFAULT_BACKLOG_PATH` walks parents to find closest
`.workingdir/BACKLOG.md`. Handles per-agent worktree case
(`<main-repo>/.claude/worktrees/agent-<id>/...`) by hopping up to
main repo root. **Don't** simplify this to
`Path.cwd().parent / ".workingdir" / "BACKLOG.md"` — breaks
worktree case silently.

## Testing

The synthetic current-schema corpus lives under `testdata/` and the legacy
table shape remains covered. Run the unit suite above, then smoke the local
editorial ledger with:

```bash
python3 -c "
from scripts.lib.backlog_tracker import BacklogTracker, explain
bk = BacklogTracker()
print('rows:', len(bk.all()))
print('open:', len(bk.list_open()))
for tid in ['T-RC1-MASTER-GREEN', 'T-RC2-BENCH-TUNE',
            'T-RC3-MODEL-RETRAIN', 'T-FIXSWEEP-REUSE-COMPLIANCE']:
    print(tid, '→', explain(tid))
"
```

The local ledger is intentionally git-ignored, so never make its current row
count a CI assertion. The smoke must return at least one item and the named
phase-order IDs above; as of the 2026-09-23 migration it returns 22 items.
