# CI overview

This page documents the fork's CI surface for contributors. The
authoritative trigger / gate behaviour lives in the workflow files
under [`.github/workflows/`](../../.github/workflows/); this doc
explains the rules a contributor needs to know without reading every
file.

## Workflows

The fork ships eight `pull_request`-triggered workflows:

| File | Purpose |
| --- | --- |
| [`docker-image.yml`](../../.github/workflows/docker-image.yml) | Docker image build (advisory). |
| [`security-scans.yml`](../../.github/workflows/security-scans.yml) | Semgrep / CodeQL / Gitleaks / Dependency Review. |
| [`lint-and-format.yml`](../../.github/workflows/lint-and-format.yml) | Pre-commit, clang-tidy, cppcheck, mypy, registry validate. |
| [`required-aggregator.yml`](../../.github/workflows/required-aggregator.yml) | Single required-check aggregator (ADR-0313). |
| [`ffmpeg-integration.yml`](../../.github/workflows/ffmpeg-integration.yml) | FFmpeg + libvmaf build (gcc / clang / SYCL / Vulkan). |
| [`libvmaf-build-matrix.yml`](../../.github/workflows/libvmaf-build-matrix.yml) | Cross-platform / cross-backend libvmaf build matrix. |
| [`rule-enforcement.yml`](../../.github/workflows/rule-enforcement.yml) | ADR-0100 / 0106 / 0108 / 0165 process gates. |
| [`tests-and-quality-gates.yml`](../../.github/workflows/tests-and-quality-gates.yml) | Netflix golden, sanitizers, tiny-AI, MCP, coverage, assertion-density. |

## Draft pull requests do not trigger CI

Per [ADR-0331](../adr/0331-skip-ci-on-draft-prs.md), every
`pull_request`-triggered workflow above is gated to skip when the PR
is in `draft` state. Concretely:

- Each workflow's `pull_request:` block lists
  `types: [opened, synchronize, reopened, ready_for_review]`.
- Each top-level job carries an `if:` clause of the form
  `github.event_name != 'pull_request' || github.event.pull_request.draft == false`.

What this means for contributors:

1. **A draft PR shows no green checks.** The required-checks
   aggregator skips on drafts and branch protection treats the
   missing aggregator as "required check absent". This is benign —
   GitHub blocks merging a draft PR by definition, so the gate cannot
   be bypassed.
2. **Promoting the draft to ready-for-review fires CI exactly once.**
   GitHub's `ready_for_review` event is what re-triggers the
   workflows; subsequent `synchronize` events on the now-ready PR
   fire CI as before.
3. **Pushing to `master` is unaffected.** The job-level `if:` clause
   short-circuits to `true` when there is no PR object (for example
   on `push:` events).

To preview CI status before merging, mark the PR ready-for-review.
You can flip back to draft afterwards if more work is needed; the next
`ready_for_review` will fire a fresh matrix.

## Required-checks aggregator

The single required check on `master` branch protection is the
**Required Checks Aggregator** (see
[ADR-0313](../adr/0313-ci-required-checks-aggregator.md)). It runs on
every non-draft PR, polls for the named sibling check_runs to reach a
terminal state, and accepts `success`, `skipped`, or `neutral` per
check. Because the aggregator itself skips on drafts, draft PRs
display "missing required check" — same situation as item 1 above
and unmergeable for the same reason.

## Bug-status hygiene gate (ADR-0165 / ADR-0334)

Per [CLAUDE.md §12 rule 13](../../CLAUDE.md) and
[ADR-0165](../adr/0165-state-md-bug-tracking.md), every PR that
closes a bug, opens a bug, or rules a Netflix upstream report
not-affecting-the-fork updates [`docs/state.md`](../state.md) in the
**same PR**. Until [ADR-0334](../adr/0334-state-md-touch-check-ci-gate.md)
this rule was reviewer-enforced; it now runs as the
`state-md-touch-check` job in
[`rule-enforcement.yml`](../../.github/workflows/rule-enforcement.yml),
backed by the single-purpose script
[`scripts/ci/state-md-touch-check.sh`](../../scripts/ci/state-md-touch-check.sh).

**The gate fires when any of the following hold:**

- PR title carries a Conventional-Commit `fix:` or `fix(scope):` prefix.
- PR title contains the bare token `bug` (word-boundary, so `debug`
  does not fire).
- PR title or body contains a `closes` / `fixes` / `resolves`
  `#N` GitHub-issue close keyword (case-insensitive).
- PR body has the `## Bug-status hygiene` template section with the
  `docs/state.md` checkbox left unchecked.

**The gate clears when either:**

1. The diff against `BASE_SHA..HEAD_SHA` includes
   [`docs/state.md`](../state.md) (the row landed in the
   appropriate section: Open / Recently closed / Confirmed
   not-affected / Deferred) **AND** none of the inserted lines
   carry a placeholder PR/commit reference (see
   "Placeholder-ref hardening" below), **or**
2. The PR description contains `no state delta: REASON` (REASON is
   any non-empty token that is not the literal placeholder
   `REASON`). Use this for pure `feat` / `refactor` / `infra` PRs
   that genuinely have no bug-status impact.

**Placeholder-ref hardening (ADR-0334 status update 2026-05-09).**
Touching `docs/state.md` is necessary but not sufficient. PR #541's
row audit found that the dominant staleness pattern is post-merge
backfill drift — closing PRs write `this PR` as the closer-PR
placeholder, the merge happens, the placeholder never gets rewritten
to the merged numeric refs. The gate therefore additionally rejects
any inserted line in `docs/state.md` containing:

| Placeholder   | Why                                         |
| ------------- | ------------------------------------------- |
| `this PR`     | post-merge backfill drift (most common)     |
| `this commit` | same drift mode for SHA-shaped refs         |
| `TBD`         | obvious fill-it-in-later marker             |
| `<PR>`        | template placeholder                        |
| `#NNN`        | template placeholder (real refs are digits) |

Canonical accept forms — explicitly NOT matched — are `PR #N` (any
positive integer) and ``commit `<sha>` `` (the SHA wrapped in
backticks). For an in-flight PR whose number is not yet final, you
can either:

1. Land the row with a placeholder, then push a follow-up commit
   that rewrites it to `PR #<number>` after `gh pr create` returns
   the number, **or**
2. Use `PR #<this-pr-number>` once GitHub has assigned it (the PR
   number is known the moment `gh pr create` exits).

**Local dry-run** (mirrors the
[`deliverables-check.sh`](../../scripts/ci/deliverables-check.sh)
pattern):

```bash
PR_TITLE="fix: foo segfault" \
PR_BODY="$(gh pr view 999 --json body -q .body)" \
  bash scripts/ci/state-md-touch-check.sh
```

Or pipe the body on stdin if `gh` isn't on `PATH`:

```bash
gh pr view 999 --json body -q .body \
  | PR_TITLE="fix: foo segfault" bash scripts/ci/state-md-touch-check.sh
```

The companion fixture script
[`scripts/ci/test-state-md-touch-check.sh`](../../scripts/ci/test-state-md-touch-check.sh)
exercises the gate against 18 cases (5 primary + 3 regression + 10
placeholder-ref). Run it after touching either script:

```bash
bash scripts/ci/test-state-md-touch-check.sh
```

## Local pre-flight gate

Before pushing, run the local subset of CI to catch the common
formatter / lint / fast-test failures:

```bash
make format-check   # clang-format + black + isort, no writes
make lint           # clang-tidy + cppcheck + iwyu + ruff + semgrep
meson test -C build --suite=fast
pre-commit run --all-files  # if .pre-commit-config.yaml hooks are installed
```

The format-check + pre-commit pair catches roughly the same surface as
`lint-and-format.yml`'s `pre-commit` job in seconds, vs. a 10-minute
CI round-trip.
