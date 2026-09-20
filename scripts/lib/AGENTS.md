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
| `backlog_tracker.py` ↔ `.workingdir/BACKLOG.md` row format | The regex parser in `_ID_PATTERN` + `_STATUS_RULES`. | If BACKLOG.md ever adds a column or renames a status word, the parser silently mis-classifies rows. Run the smoke (`python3 -c 'from scripts.lib.backlog_tracker import BacklogTracker; print(len(BacklogTracker().all()))'`) after any structural BACKLOG.md edit; expected ≥ 100 rows on master at 2026-05-09. |
| `GitHubTracker._run` | Wraps the `gh` CLI. | Output schema (`number / title / body / headRefName / mergedAt / state`) is `gh`-version-coupled. Pin behaviour by passing `--json` field lists explicitly; never rely on default columns. |
| `safe_subprocess.py` | Python automation under `scripts/`. | Every executable is allowlisted, argv/environment/cwd are validated, captured output and runtime are bounded, and POSIX timeout cleanup owns the process group. Do not replace it with a raw `subprocess` call or an `S603` waiver. |

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

## Worktree-aware path resolution

`DEFAULT_BACKLOG_PATH` walks parents to find closest
`.workingdir/BACKLOG.md`. Handles per-agent worktree case
(`<main-repo>/.claude/worktrees/agent-<id>/...`) by hopping up to
main repo root. **Don't** simplify this to
`Path.cwd().parent / ".workingdir" / "BACKLOG.md"` — breaks
worktree case silently.

## Testing

No pytest harness for this module today (one PR doesn't buy fixture
corpus). Smoke = [Research-0091
§"Smoke results"](../../docs/research/0091-symphony-spec-review.md#smoke-results-2026-05-09),
reproducible via:

```bash
python3 -c "
from scripts.lib.backlog_tracker import BacklogTracker, explain
bk = BacklogTracker()
print('rows:', len(bk.all()))
print('open:', len(bk.list_open()))
for tid in ['T0-1', 'T3-7', 'T7-5', 'TA-VOCAB']:
    print(tid, '→', explain(tid))
"
```

Real test corpus justified (e.g. format migration or CI gate against
parser) -> add `scripts/lib/test_backlog_tracker.py` with synthetic
BACKLOG fixtures under `scripts/lib/testdata/`.
