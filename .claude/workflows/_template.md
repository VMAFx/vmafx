<!-- markdownlint-disable MD003 MD007 MD026 MD041 -->
<!--
  .claude/workflows/_template.md — canonical agent-dispatch workflow
  template. Symphony-inspired (openai/symphony §4.1.2 / §4.1.3): the
  brief is a versioned, in-repo file with typed YAML front matter so
  the dispatch contract isn't re-invented per session.

  See ADR-0355 (Symphony-inspired agent-dispatch infrastructure) for
  the rationale.
-->
---

# YAML front matter — typed dispatch contract.

# Every key below is consumed by the eligibility precheck script

# (`scripts/ci/agent-eligibility-precheck.py`) and/or by humans

# briefing an Agent(...) invocation.

# Identity ---------------------------------------------------------

name: _template
description: >
  Generic skeleton — clone this file to a task-specific instance
  (codeql-alert-sweep, simd-port, feature-extractor-port, ...).

# Dispatch ---------------------------------------------------------

agent_type: general-purpose            # or simd-reviewer, vulkan-reviewer, etc.
isolation: worktree                    # ALWAYS — per feedback_agents_isolated_worktree_only
worktree_drift_check: true             # require `pwd` verification at session start

# Deliverables (per ADR-0108) --------------------------------------

required_deliverables:

  - changelog
  - rebase_note

# Opt-out options (omit from the list above and supply a

# `no <item> needed: REASON` sentinel in the PR body):

# - digest          → "no digest needed: REASON"

# - alternatives    → "no alternatives: REASON"

# - agents_md       → "no rebase-sensitive invariants"

# - reproducer      → never opt out — always supply a command

# - changelog       → "no changelog needed: REASON"

# - rebase_note     → "no rebase impact: REASON"

# Verification gates -----------------------------------------------

verification:
  netflix_golden: true                 # CPU-only golden gate (CLAUDE §8)
  cross_backend_places: 4              # required if SIMD/GPU touched (ADR-0138/0139)
  meson_test: true                     # `meson test -C build --suite=fast`

# Forbidden actions ------------------------------------------------

forbidden:

  - modify_netflix_golden_assertions   # CLAUDE §12 r1 — never
  - lower_test_thresholds              # feedback_no_test_weakening
  - skip_lint_upstream                 # feedback_no_lint_skip_upstream

# Pre-dispatch eligibility gate ------------------------------------

master_status_check: true              # bail if master CI is currently red
backlog_id: null                       # set to "T3-9", "T7-10b", etc. for the precheck
---

# {{TITLE}} — agent prompt template

> **MUST RUN BEFORE DISPATCH** — call the eligibility precheck:
>
> ```bash
> scripts/ci/agent-eligibility-precheck.py --backlog-id "{{BACKLOG_ID}}"
> ```
>
> If it exits non-zero, do **not** dispatch. The script prints the
> closing PR / colliding agent task to stderr.

## Worktree-isolation prelude

Every brief begins with the same prelude. Do not edit per-agent —
the harness reads this verbatim for the worktree-drift gate.

```bash
AGENT_WT="$(pwd)"                     # snapshot at session start
[ -n "$AGENT_WT" ] || { echo "drift: pwd empty" >&2; exit 1; }
case "$AGENT_WT" in
  */worktrees/*) ;;                   # OK — isolated worktree
  *) echo "drift: not in a worktree ($AGENT_WT)" >&2; exit 1 ;;
esac
```

Verify before every long-running command:

```bash
[ "$(pwd)" = "$AGENT_WT" ] || { echo "drift: cwd moved" >&2; exit 1; }
```

## Task

{{TASK_BODY}}

## Constraints

- Worktree isolation: `AGENT_WT=$(pwd)`, absolute paths only.
- CLAUDE.md §12 r1: never modify Netflix golden assertions.
- CLAUDE.md §12 r10: every user-discoverable surface ships docs in
  the same PR.
- CLAUDE.md §12 r11: six deep-dive deliverables (research digest,
  decision matrix, AGENTS.md note, reproducer, changelog, rebase
  note) — opt out per-item with `no <item> needed: REASON`.
- `feedback_deliverables_checklist_strict_parser`: UN-tick the
  checkbox AND add the sentinel — never `[x] *deferred*`.

## Deliverables

{{DELIVERABLES_LIST}}

## Return shape

- Branch name + PR URL.
- Sample invocations of any new tooling.
- The exact reproducer command from the PR body.
