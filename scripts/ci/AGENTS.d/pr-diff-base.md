---
paths:
  - scripts/ci/pr-diff-base.sh
  - scripts/ci/classify-dependency-pr.sh
  - scripts/ci/release-pr-exempt.sh
  - scripts/ci/tests/test_pr_gates_merge_base.py
invariant: PR gates diff from merge base of BASE_SHA and HEAD_SHA, never from BASE_SHA; no merge base -> fail closed.
area: gates
---
<!-- markdownlint-disable MD013 MD060 -->
# Pull request diff base (`pr-diff-base.sh`)

`BASE_SHA` = `github.event.pull_request.base.sha` = base branch tip when event
fired, not fork point. Two-dot diff from that tip = PR's change plus every file
master changed since fork, rendered files (`CHANGELOG.md`, ADR index) too.
Defect seen: Deliverables Checklist refused PRs that fell behind master
(ADR-2197 rendered-file error); `docs/state.md` gate passed PR on state row
master added.

- `pr_diff_base BASE HEAD CALLER` (sourced, never executed) prints
  `git merge-base BASE HEAD`; diff = merge base `..` HEAD (three-dot
  meaning). Callers: `deliverables-check.sh` (rendered files, small-PR
  counter, ticked-file check), `state-md-touch-check.sh`,
  `ffmpeg-patches-surface-check.sh`, `classify-dependency-pr.sh`,
  `release-pr-exempt.sh`.
- No merge base (shallow checkout, missing commit, unrelated histories) ->
  stderr names caller, gates exit 2; `classify-dependency-pr.sh` exit 2 (not
  exempt); `release-pr-exempt.sh` lists no path -> `exempt=false`. Never
  fall back to diff from `BASE_SHA`.
- Workflow jobs that pass `BASE_SHA` check out with `fetch-depth: 0`
  (`rule-enforcement.yml`). Tier workflows run `release-pr-exempt.sh` from
  sparse checkout with `DIFF_FILE`; script sources `pr-diff-base.sh` only in
  `BASE_SHA` branch, so sparse lists need no new file.
- `validate-pr-body.sh` shim answers `merge-base validator-base
  validator-head` with `validator-base`.
- Not this defect: `check-silent-revert.py` merges head into base tip on
  purpose; `check-dco.py` reads commit range `base..head` (rev-list
  excludes base's history).
- Test: `tests/test_pr_gates_merge_base.py` (behind-base, own-edit,
  fail-closed cases; 8 fail against two-dot scripts).
