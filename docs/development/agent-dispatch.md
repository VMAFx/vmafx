<!-- markdownlint-disable MD013 MD060 -->
# Agent dispatch — workflow templates and eligibility precheck

> Operational guide for briefing and dispatching Claude Code agents
> against fork-local backlog items. Implements [ADR-0355](../adr/0355-symphony-agent-dispatch-infra.md);
> see [Research-0091](../research/0091-symphony-spec-review.md) for
> the design rationale (which Symphony shapes were adopted, which
> were deliberately dropped).

This page covers three things, in dispatch order:

1. **Pick a workflow template** under `.claude/workflows/`.
2. **Run the eligibility precheck** to confirm the work isn't already
   done or already in flight.
3. **Brief the agent** by filling the template's `{{...}}` slots.

## 1. Workflow templates

Every recurring agent task class has a template under
`.claude/workflows/`:

| File | Use it when |
|---|---|
| [`_template.md`](../../.claude/workflows/_template.md) | Generic skeleton. Clone for any new task class. |
| [`codeql-alert-sweep.md`](../../.claude/workflows/codeql-alert-sweep.md) | Bulk-fix N CodeQL alerts in a single category. |
| [`simd-port.md`](../../.claude/workflows/simd-port.md) | Port or audit a SIMD path (AVX-512 widening, NEON sister, AVX2 audit). |
| [`feature-extractor-port.md`](../../.claude/workflows/feature-extractor-port.md) | Port a feature extractor to a GPU backend (CUDA / SYCL / HIP / Vulkan). |

Each template starts with **typed YAML front matter** (Symphony
§4.1.2 / §4.1.3 shape) that captures the dispatch contract:

```yaml
name: codeql-alert-sweep
description: Bulk-fix CodeQL alerts in one category
agent_type: general-purpose
isolation: worktree
worktree_drift_check: true
required_deliverables: [changelog, rebase_note]
verification:
  netflix_golden: true
  cross_backend_places: 4
  meson_test: true
forbidden:
  - modify_netflix_golden_assertions
  - lower_test_thresholds
master_status_check: true
backlog_id: T3-9
```

The body below the front matter is the prompt template. `{{...}}`
markers are placeholder slots filled at dispatch time (e.g.
`{{ALERT_LIST}}`, `{{BACKLOG_ID}}`). The body is plain Markdown — no
DSL, no execution engine — so it can be re-edited by hand without
touching tooling.

### Adding a new template

1. Copy `_template.md` to `.claude/workflows/<task-class>.md`.
2. Edit the front matter — at minimum, set `name` to the file's
   basename, write a one-line `description`, and decide which
   `required_deliverables` and `forbidden` fields apply.
3. Replace the body's `{{TASK_BODY}}` and `{{DELIVERABLES_LIST}}`
   placeholders with task-specific guidance.
4. Land the new template in the same PR as the first dispatch that
   uses it — **never** ship a template without a worked example.

## 2. Eligibility precheck — `agent-eligibility-precheck.py`

> **MUST RUN BEFORE EVERY DISPATCH.**

The script lives at
[`scripts/ci/agent-eligibility-precheck.py`](../../scripts/ci/agent-eligibility-precheck.py).
It runs three checks:

1. **BACKLOG row not closed.** Parses
   `.workingdir2/BACKLOG.md` via
   [`scripts/lib/backlog_tracker.py`](../../scripts/lib/backlog_tracker.py).
   If the row's status is DONE / CLOSED / REMOVED / BLOCKED /
   DEFERRED, exit 1.
2. **No merged PR mentions this scope.** Calls `gh pr list --search
   "<id> in:title,body" --state merged`. Any hit means the work has
   likely already shipped — exit 1 and list the matching PRs.
3. **No in-flight agent on the same scope.** Scans
   `/tmp/claude-<uid>/*/tasks/*.output` (the harness's per-task
   metadata) plus the open-PR head-branch list for any active task
   that mentions the scope. If found, exit 1.

Verdicts go to **stderr** in GitHub Actions `::error` format so a
wrapping CI script can parse them:

```bash
$ python3 scripts/ci/agent-eligibility-precheck.py --backlog-id T0-1 --skip-gh-search --skip-active-scan
agent-eligibility-precheck: scope=T0-1
::error title=agent-eligibility: T0-1 is DONE::BACKLOG.md row already closed (PRs: #72). Title: Port cuMemFreeAsync → cuMemFree
agent-eligibility-precheck: VERDICT=FAIL — do not dispatch.

$ echo "exit=$?"
exit=1
```

```bash
$ python3 scripts/ci/agent-eligibility-precheck.py --backlog-id T3-7 --skip-gh-search --skip-active-scan
agent-eligibility-precheck: scope=T3-7
  backlog: T3-7 status=OPEN priority=3 — OK
agent-eligibility-precheck: VERDICT=PASS — dispatch eligible.

$ echo "exit=$?"
exit=0
```

### Flags

| Flag | Meaning |
|---|---|
| `--backlog-id ID` | Run all three checks against the given BACKLOG row. |
| `--task-tag TAG` | Free-form scope tag for runs without a backlog row (e.g. `codeql-cpp-overflow`). Skips checks 1 and 2. |
| `--skip-gh-search` | Skip check 2 (merged-PR search). Useful offline. |
| `--skip-active-scan` | Skip check 3 (in-flight scan). |
| `--repo OWNER/REPO` | GitHub repository (default: `VMAFx/vmafx`). |
| `--harness-tasks-glob GLOB` | Override the harness task-files glob (default: `/tmp/claude-<uid>/*/tasks/*.output`). |
| `--backlog-path PATH` | Override the BACKLOG.md path (default: autodetect from the worktree's main repo). |

### Wiring it in

The Claude Code harness does not currently expose a pre-`Agent`
dispatch hook in `settings.json`. Until it does, calling the
precheck is **manual** — it sits at the top of every workflow
template's body. When the harness gains the hook, register the
precheck via `.claude/settings.json` and remove the manual call.

## 3. Tracker abstraction — `scripts/lib/backlog_tracker.py`

The precheck and any future state-audit script consume BACKLOG.md
through a typed module rather than re-grepping the file:

```python
from lib.backlog_tracker import BacklogTracker, GitHubTracker, BacklogItem

bk = BacklogTracker()                    # autodetects .workingdir2/BACKLOG.md

bk.list_open()                           # -> list[BacklogItem]
bk.list_in_flight()                      # -> list[BacklogItem]
bk.list_closed()                         # -> list[BacklogItem]
bk.get("T3-9")                           # -> BacklogItem | None

gh = GitHubTracker(repo="VMAFx/vmafx")
gh.merged_prs_since(some_datetime)       # -> list[dict]
gh.search_prs("T3-9 in:title,body")      # -> list[dict]
gh.open_agent_branches()                 # -> list[str]
```

`BacklogItem` exposes:

```python
@dataclass
class BacklogItem:
    id: str            # "T3-9", "T7-10b", "TA-VOCAB"
    title: str         # markdown-stripped first cell
    status: str        # OPEN | IN_FLIGHT | DONE | CLOSED | REMOVED | BLOCKED | DEFERRED
    priority: int | None   # tier number (T0=0, T7=7, TA-* = None)
    pr_refs: list[int]     # PR numbers parsed from "PR #N" markers
    raw_row: str           # original markdown row, for diagnostics
```

The module is read-only — **never** writes to BACKLOG.md. Edits
remain a manual editorial task per the global "Read AND update local
state files" rule. If `.workingdir2/` ever migrates off Markdown
(JSON, SQLite, Linear), this module is the only file that needs to
change.

## See also

- [ADR-0355](../adr/0355-symphony-agent-dispatch-infra.md) — decision rationale.
- [Research-0091](../research/0091-symphony-spec-review.md) — what we adopted from Symphony and what we dropped.
- [ADR-0108](../adr/0108-deep-dive-deliverables-rule.md) — the six-deliverable rule encoded in `required_deliverables`.
- [`scripts/ci/AGENTS.md`](../../scripts/ci/AGENTS.md) — invariants for files in `scripts/ci/`.
- [`scripts/lib/AGENTS.md`](../../scripts/lib/AGENTS.md) — invariants for the tracker abstraction.
