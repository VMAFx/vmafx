<!-- markdownlint-disable MD018 -->
# Research-0090 — Pre-push PR-body deliverables validator

**Date**: 2026-05-09
**Author**: feedback loop on the strict deliverables-checklist parser
**Status**: implemented (this PR)

## Problem

The `.github/workflows/rule-enforcement.yml` deep-dive-checklist job
runs `scripts/ci/deliverables-check.sh` against the PR body on every
non-draft `pull_request` event. The parser is intentionally strict:

- Recognises only `- [x]` checkboxes, not numbered lists.
- Expects bold-bracket label substring matches (e.g.
  `**Reproducer / smoke-test command**`, not
  `**Reproducer / smoke-test**`).
- Cross-checks ticked items against the diff so a ticked
  "Research digest" without a `docs/research/NNNN-*.md` in the diff
  fails.
- Cross-checks "CHANGELOG fragment" against `^changelog\.d/<sec>/`
  paths and "Rebase note" against `^docs/rebase-notes\.md$`.

In practice, the strict parser caused ≥7 retries per session on PRs
#461, #438, #470, #473, #486, #511, #468, and #526. Every retry
costs a 3–10 minute CI cycle.

## Goal

Catch parser failures **locally** before push, not after a CI
round-trip.

## Options considered

### A — Parser duplication in a Python pre-push hook

Re-implement the parser in Python (or Go) as a separate module.

- Pros: prettier diagnostics, type-checked options, more flexible.
- Cons: **two parsers to keep in sync**. Drift between local and CI
  parsers would re-introduce the exact failure mode the hook is
  meant to prevent. Direct violation of memory
  `feedback_no_guessing` ("re-use existing parser; don't fork").

**Rejected.**

### B — Wrap `deliverables-check.sh` and inject diff via env vars

Discovered that `deliverables-check.sh` already supports
`PR_BODY` env var injection but uses `git diff` to fetch the diff
from `BASE_SHA..HEAD_SHA`. Arbitrary file lists cannot be passed
through that interface directly.

Workaround: shadow `git` on `PATH` for the child process via a
shim that returns the pre-computed file list when called with
`diff --name-only` and falls through to the real `git` binary
otherwise.

- Pros: zero parser duplication. Single source of truth in
  `deliverables-check.sh`. Testable in isolation. The shim is ~10
  lines of bash and only intercepts the one call shape.
- Cons: PATH-shim is mildly clever. Requires a small note in the
  validator script header pointing to where the indirection
  happens.

**Selected.**

### C — Modify `deliverables-check.sh` to accept a `--diff` flag

Cleaner long-term, but mutates the file the rule-enforcement
workflow runs in CI. Larger blast radius if the modification has
a bug; touches a workflow-load-bearing file unnecessarily.

- Pros: no shim.
- Cons: bigger PR; couples the validator's UX evolution to changes
  in the CI gate's interface.

**Deferred.** If a follow-up PR ever needs richer diff sourcing
(e.g. a JSON file list for parallel validation), that's the time
to add the flag.

## Implementation summary

- `scripts/ci/validate-pr-body.sh` — standalone CLI, accepts
  `--body PATH` and `--diff PATH`, falls back to stdin / merge-base.
  Builds a temp-dir `git` shim that intercepts `diff --name-only`,
  invokes `deliverables-check.sh` with `PR_BODY` env var.
- `scripts/git-hooks/pre-push` — git hook. No-op when `gh` is
  missing, no PR exists, PR is draft / merged / closed, or body is
  empty. Otherwise invokes the validator.
- `Makefile` `hooks-install` — adds an idempotent symlink
  `.git/hooks/pre-push -> scripts/git-hooks/pre-push`. Preserves
  pre-existing non-symlink hooks as `.local-backup`.
- `scripts/ci/test-validate-pr-body.sh` — eight cases covering
  pass-when-files-present, fail-when-ticked-without-file
  (Research / CHANGELOG / Rebase note), fail-on-numbered-list,
  fail-on-no-sentinel, pass-on-tick-with-redundant-sentinel,
  pass-on-all-opt-out-sentinels.
- `docs/development/pr-body-validator.md` — operator-facing doc.

## Validation

8/8 test cases pass:

```text
PASS: ticked + files present in diff (exit=0)
PASS: ticked Research digest, no file in diff (exit=1)
PASS: numbered-list shape (no - [x]) (exit=1)
PASS: unticked, no opt-out sentinel (exit=1)
PASS: sentinel + ticked (parser-permissive) (exit=0)
PASS: all six opted-out via sentinels (exit=0)
PASS: ticked CHANGELOG, no fragment in diff (exit=1)
PASS: ticked Rebase note, no rebase-notes.md in diff (exit=1)
```

Sample failure output (numbered-list shape, empty diff) is
verbatim the same `::error title=ADR-0108 …::…` lines the CI gate
emits — confirming parser parity.

## Caveats

The validator's local pass is **not** a guarantee that the CI gate
passes; CI uses the PR-object's `BASE_SHA..HEAD_SHA` while the
hook uses `merge-base origin/master..HEAD`. These usually agree
but can diverge on stale local refs. Documented explicitly in the
operator doc per memory `feedback_no_test_weakening` ("local pass
is not a substitute for CI").
