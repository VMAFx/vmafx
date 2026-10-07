---
paths:
  - scripts/ci/validate-pr-body.sh
  - scripts/ci/deliverables-check.sh
  - scripts/ci/test-validate-pr-body.sh
  - scripts/git-hooks/pre-push
  - scripts/git-hooks/pre-push-pr-body-lint.sh
  - scripts/git-hooks/test-pre-push-pr-body-lint.py
invariant: `deliverables-check.sh` is the only parser; the validator shims `git diff --name-only` and nothing else.
---
<!-- markdownlint-disable MD013 MD060 -->
# PR-body deliverables validator (`validate-pr-body.sh`)

`scripts/ci/validate-pr-body.sh`, `scripts/git-hooks/pre-push`, and
`scripts/git-hooks/pre-push-pr-body-lint.sh` = local mirrors of
`.github/workflows/rule-enforcement.yml` deep-dive-checklist gate
(ADR-0108). Re-use `scripts/ci/deliverables-check.sh` verbatim as
parser; validator only injects diff via `PATH`-shim that
intercepts `git diff --name-only`.

`pre-push-pr-body-lint.sh` = standalone entry point referenced by
the `.pre-commit-config.yaml` `validate-pr-body` hook (`stages:
[pre-push]`). The omnibus `pre-push` hook delegates to same
validator logic. Its `gh` lookup is time-bounded; unavailable credentials fall
back to the public PR list and page. Only a confirmed no-open-PR result skips
validation. Indeterminate metadata fails closed. Preserve
`test-pre-push-pr-body-lint.py` in both commit and push hooks so a locked keyring
cannot restore the unbounded hang or an authentication failure bypass.

The hook skips the machine-generated release PR only by calling
`release-pr-exempt.sh` (ADR-1151), the same predicate CI's Deliverables
Checklist calls; never re-implement it in the hook. It feeds the predicate the
PR's `headRefName` and an author mapped from `gh pr view --json author` to the
event-payload shape CI passes: `{"is_bot": true, "login": "app/<x>"}` ->
`PR_AUTHOR=<x>[bot]`, `PR_AUTHOR_TYPE=Bot`; `{"is_bot": false, "login": "<x>"}`
-> `<x>`, `User`; any other shape (deleted author, missing field) -> empty,
which never exempts. The public-page fallback carries no author and must never
exempt; a branch without the predicate validates as before. The
`test-pr-body-lookup` hook re-runs on predicate edits and covers the bot,
human-same-ref, non-bot-lookalike and fallback cases.

**Invariant — single parser source of truth**: do not fork or
re-implement deliverables-check parsing logic in any other
language. If gate's regex shape ever changes, change lands
in `deliverables-check.sh`, validator picks it up
automatically. Test harness `test-validate-pr-body.sh` should
catch any drift between validator's expectations and
parser's actual behaviour.

**Invariant — shim scope**: `git` shim built inside
`validate-pr-body.sh` intercepts only `diff --name-only` call
shape. Every other `git` invocation falls through to real
binary. Future change to `deliverables-check.sh` using
different git subcommand to compute diff must update shim.
Else `validate-pr-body.sh` silently uses real
git's output — potentially fine, potentially wrong depending on
local repo state.

**Invariant — rendered files (ADR-2197)**: `deliverables-check.sh` refuses
PR diff touching `CHANGELOG.md`, `docs/adr/README.md`, `docs/adr/titles.md`,
`docs/research/titles.md`, `docs/adr/by-tag/*`, `_order.txt`,
`docs/rebase-notes.md`. Those = `make docs-render` output, written by merge
train per batch and by release cut. PR adds fragments only
(`changelog.d/`, `docs/adr/_index_fragments/`, `docs/rebase-notes.d/`).
Ticked CHANGELOG item needs `changelog.d/<section>/*.md`; ticked rebase-note
item needs `docs/rebase-notes.d/<slug>.md`. Render set lives in `Makefile`
(`docs-render`, `docs-render-check`) and this gate's `rendered_paths`: change
both together. `test-validate-pr-body.sh` plants each refused path.
