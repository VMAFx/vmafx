- **state.md-touch-check CI gate
  ([ADR-0334](../docs/adr/0334-state-md-touch-check-ci-gate.md)).**
  Promotes [CLAUDE.md §12 rule 13](../CLAUDE.md) /
  [ADR-0165](../docs/adr/0165-state-md-bug-tracking.md) from
  reviewer-enforced to CI-enforced. New blocking job
  `state-md-touch-check` in
  [`.github/workflows/rule-enforcement.yml`](../.github/workflows/rule-enforcement.yml)
  fires `scripts/ci/state-md-touch-check.sh` on every non-draft PR.
  The gate trips when a PR looks bug-shaped — Conventional-Commit
  `fix:` / `fix(scope):` prefix, the bare token `bug` in the title
  (word-boundary so `debug` does not fire), GitHub-issue close
  keywords (`closes` / `fixes` / `resolves` `#N`), or the PR body's
  Bug-status-hygiene template row left unchecked — AND the diff
  does not touch [`docs/state.md`](../docs/state.md) AND the body
  does not carry the explicit opt-out `no state delta: REASON`.
  HTML comments are stripped from the body before the opt-out match
  so the template's instructional placeholder does not accidentally
  satisfy the gate. Companion fixture script
  `scripts/ci/test-state-md-touch-check.sh` covers eight cases
  (5 primary + 3 regression: `debug` vs `bug`, `Closes #N`, upper-
  case `BUG-`). Local dry-run:
  `PR_TITLE='fix: foo' PR_BODY="$(gh pr view N --json body -q .body)" bash scripts/ci/state-md-touch-check.sh`.
  Mirrors the script-with-thin-wrapper shape of
  `scripts/ci/deliverables-check.sh` (ADR-0124). Surfaced as a
  backlog row by the state.md audit-backfill PR #455.
