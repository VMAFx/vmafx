<!-- markdownlint-disable MD060 -->
# `scripts/lib/` — agent invariants

Parent: [../AGENTS.md](../AGENTS.md).

Shared Python utilities consumed by `scripts/ci/` and (in future)
by `scripts/dev/` and `.github/workflows/*.yml`. Created by
[ADR-0355](../../docs/adr/0355-symphony-agent-dispatch-infra.md);
see [Research-0091](../../docs/research/0091-symphony-spec-review.md)
for design rationale.

## Rebase-sensitive surfaces

Nothing in this directory mirrors upstream Netflix/vmaf. The rebase
risk is "internal coupling drift" rather than "merge conflict".

| Module | Consumers | What couples them |
|---|---|---|
| `backlog_tracker.py` | `scripts/ci/agent-eligibility-precheck.py` (direct import); future state-audit / status-reporter scripts. | The `BacklogItem` dataclass field names (`id` / `title` / `status` / `priority` / `pr_refs` / `raw_row`) and the status enum strings (OPEN / IN_FLIGHT / DONE / CLOSED / REMOVED / BLOCKED / DEFERRED). Renames are breaking changes for every importer. |
| `backlog_tracker.py` ↔ `.workingdir2/BACKLOG.md` row format | The regex parser in `_ID_PATTERN` + `_STATUS_RULES`. | If BACKLOG.md ever adds a column or renames a status word, the parser silently mis-classifies rows. Run the smoke (`python3 -c 'from scripts.lib.backlog_tracker import BacklogTracker; print(len(BacklogTracker().all()))'`) after any structural BACKLOG.md edit; expected ≥ 100 rows on master at 2026-05-09. |
| `GitHubTracker._run` | Wraps the `gh` CLI. | Output schema (`number / title / body / headRefName / mergedAt / state`) is `gh`-version-coupled. Pin behaviour by passing `--json` field lists explicitly; never rely on default columns. |

## Read-only invariant

`backlog_tracker.py` **never** writes to BACKLOG.md. Adding a write
path is a CLAUDE.md global-rule violation ("Read AND update local
state files" — the *update* half is editorial, not automated). If a
genuine machine-write is ever needed, it lands in a separate module
(e.g. `backlog_writer.py`) with its own ADR.

## Stdlib-only invariant

The module imports only from the Python standard library
(`os`, `re`, `subprocess`, `dataclasses`, `datetime`, `pathlib`,
`json`, `typing`). Adding a third-party dependency (PyYAML, Linear
SDK, etc.) is a regression for two reasons:

1. The fork's CI lint profile runs against system Python; new wheels
   add wheels-cache surface and Sigstore-signing toil.
2. The point of this module is to be importable from any wrapper
   script without a virtualenv setup.

If a new dep is genuinely justified, write an ADR first.

## Worktree-aware path resolution

`DEFAULT_BACKLOG_PATH` walks parents to find the closest
`.workingdir2/BACKLOG.md`. Importantly, it handles the per-agent
worktree case (`<main-repo>/.claude/worktrees/agent-<id>/...`) by
hopping up to the main repo root. **Don't** simplify this to
`Path.cwd().parent / ".workingdir2" / "BACKLOG.md"` — that breaks
the worktree case silently.

## Testing

There is no pytest harness for this module today (one PR doesn't buy
a fixture corpus). The smoke is in [Research-0091
§"Smoke results"](../../docs/research/0091-symphony-spec-review.md#smoke-results-2026-05-09)
and reproducible via:

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

When a real test corpus is justified (e.g. format migration or a CI
gate against the parser), add `scripts/lib/test_backlog_tracker.py`
with synthetic BACKLOG fixtures under `scripts/lib/testdata/`.
