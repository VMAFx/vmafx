<!-- markdownlint-disable MD007 MD013 MD041 -->
<!--
  Workflow: bulk-fix CodeQL alerts in a single category. Reuse for
  every recurring "fix N alerts of category X" agent run instead of
  re-briefing in prose.
-->
---
name: codeql-alert-sweep
description: Bulk-fix CodeQL alerts in one category
agent_type: general-purpose
isolation: worktree
worktree_drift_check: true
required_deliverables:

  - changelog
  - rebase_note
verification:
  netflix_golden: true
  cross_backend_places: 4              # only if a SIMD/GPU file gets edited
  meson_test: true
forbidden:
  - modify_netflix_golden_assertions
  - lower_test_thresholds
  - blanket_nolint_suppress            # CodeQL is severity-tiered; suppress only with cite
master_status_check: true
backlog_id: null                       # CodeQL sweeps usually have no backlog row

---

# CodeQL alert sweep — {{ALERT_CATEGORY}}

> **MUST RUN BEFORE DISPATCH** (skip if `backlog_id: null`):
>
> ```bash
> # CodeQL sweeps don't have a BACKLOG.md row; instead check that
> # no other agent is already on the same alert category:
> scripts/ci/agent-eligibility-precheck.py \
>     --task-tag "codeql-{{ALERT_CATEGORY}}"
> ```

## Worktree-isolation prelude

(see `_template.md`)

## Task

Fix the following CodeQL alerts (category: **{{ALERT_CATEGORY}}**):

{{ALERT_LIST}}

For each alert:

1. Read the surrounding code; classify the alert as TP (real bug),
   FP (false positive — needs `// codeql[…]` suppression with cite),
   or refactor-required.
2. **Never** suppress without a one-line justification comment naming
   the ADR / research digest / rebase invariant that forces the
   suppression. A bare `// codeql[js/path-injection]` is itself a
   lint violation under CLAUDE.md §12 r12 (touched-file cleanup).
3. Run `make lint` after every batch of 3-5 fixes; bisect locally
   if a new finding appears.

## Constraints

- Worktree isolation per `_template.md`.
- CLAUDE.md §12 r1: never weaken golden tests.
- `feedback_no_test_weakening`: do not lower CodeQL severity gates.

## Deliverables

- Per-alert one-liner in the PR body explaining TP/FP/refactor
  verdict + the new line:column where the fix lands.
- Changelog fragment under `changelog.d/security/{{topic}}.md`
  (or `changelog.d/fixed/` for non-security alerts).
- Rebase note entry under a new ID in `docs/rebase-notes.md`
  (only if a fix touches an upstream-mirror file; opt out otherwise
  with `no rebase impact: fork-local file only`).
- Reproducer: `gh codeql ... view-alert <id>` + the post-fix
  `make lint` output proving the alert is no longer reported.

## Return shape

- Branch name + PR URL.
- Sample alert: TP/FP verdict + line:column of the fix.
