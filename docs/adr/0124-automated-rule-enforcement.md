<!-- markdownlint-disable MD060 -->
# ADR-0124: Automate enforcement of process ADRs (0100 / 0105 / 0106 / 0108)

- **Status**: Accepted
- **Date**: 2026-04-20
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ci, agents, framework, docs, license

## Context

Four rule-bearing ADRs currently rely on reviewer discipline and
session-start checklists rather than automated gates:

- [ADR-0100](0100-project-wide-doc-substance-rule.md) — every PR that
  touches a user-discoverable surface ships `docs/` in the same PR.
- [ADR-0105](0105-copyright-handling-dual-notice.md) — dual-notice
  copyright header policy on new `.c` / `.h` / `.cpp` / `.cu` files.
- [ADR-0106](0106-adr-maintenance-rule.md) — every non-trivial decision
  gets its own `docs/adr/NNNN-*.md` file before the commit that lands
  it.
- [ADR-0108](0108-deep-dive-deliverables-rule.md) — every fork-local PR
  ships the six deep-dive deliverables.

Recent PRs (#47, #60, #62) demonstrate *de facto* compliance, but the
rules are voluntary: a contributor (human or agent) who skips the PR
template or forgets a deliverable is not blocked. As the contributor
base grows — and as autonomous-agent workflows become routine — the
next regression is a question of when, not whether.

The session-start ADR-backfill discipline (read
[`docs/adr/README.md`](README.md) every session, check for missing
files) is not automatable without noise: a "decision keyword" scan of
commit messages surfaces mostly false positives. Keep that one as
discipline; automate the other three with low-friction checks that
fail fast on PR.

## Decision

Add a single `.github/workflows/rule-enforcement.yml` workflow that
runs three jobs on every PR, plus a pre-commit hook for the copyright
policy:

1. **`doc-substance-check`** (ADR-0100) — **advisory**. Greps the PR
   diff for user-discoverable surface changes (paths enumerated in the
   ADR's `What counts` section) and posts a PR comment listing
   surfaces changed without a matching `docs/` diff. Non-blocking; the
   signal is a reviewer checklist, not a merge gate.

2. **`deep-dive-checklist`** (ADR-0108) — **blocking**. Parses the PR
   description for the six checkboxes declared by
   [`.github/PULL_REQUEST_TEMPLATE.md`](../../.github/PULL_REQUEST_TEMPLATE.md)
   under the "Deep-dive deliverables" heading. Each checkbox must
   either be ticked or replaced with a `no <item> needed: <reason>` /
   `no rebase impact: <reason>` opt-out line. Exempts PRs whose commit
   title starts with `port:` or whose base-branch name starts with
   `port/` (the existing `port-upstream-commit` convention). Fails the
   check if any of the six items is neither ticked nor opted-out; fails
   if a ticked `research digest` / `rebase note` item references a
   file path that doesn't appear in the PR diff. Non-fork-local PRs
   (pure upstream syncs) are exempted via the `port:` label.

3. **`adr-backfill-check`** (ADR-0106) — **advisory**. Runs once per
   PR. Lists ADR files added in the PR diff; if zero and the diff
   touches `core/include/`, `meson_options.txt`, `.github/`, or any
   `docs/principles.md` edit (structural / policy surface), posts a
   comment asking whether a new ADR is required. Non-blocking by
   design — the false-positive rate on a pure keyword scan is too high
   to gate on.

4. **Copyright pre-commit hook** (ADR-0105) — `scripts/check-copyright.sh`
   invoked from `.pre-commit-config.yaml`. Runs on staged `*.c` /
   `*.h` / `*.cpp` / `*.cu` / `*.cuh` files. Rejects files without any
   `Copyright` line and flags files where the year-range or owner
   doesn't match one of the three approved templates (Netflix-only on
   pre-existing upstream paths; Lusoris+Claude-only on fork-authored
   subtrees; dual on fork-modified upstream paths). Routes errors
   through the standard pre-commit output so `git commit` surfaces
   them before push.

The workflow is purpose-named per [ADR-0116](0116-ci-workflow-naming-convention.md)
and is pinned as a required status check only for the `deep-dive-checklist`
job (the blocking one). The two advisory jobs are not pinned; they
surface as PR comments for the reviewer to weigh.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| One PR per ADR (four separate PRs) | Smallest reviewable unit per PR | Four rounds of six-deliverable overhead; four rebases against master; the four gates share enough CI / docs scaffolding that splitting duplicates setup | Rejected per prior user preference for bundled refactor PRs in shared surfaces (validated on the ADR file-per-decision migration) |
| Make all three jobs blocking | Strongest signal | False-positive rate on `doc-substance` and `adr-backfill` would block real work — the "user-discoverable" predicate is fuzzy (e.g. internal `feature/*.c` refactor), and the decision-keyword scan catches refactor commits that don't warrant an ADR | Rejected — prefer advisory comments over noisy blocks; escalate specific jobs to blocking after evidence |
| Skip ADR-0106 automation entirely | Zero false-positive risk | Rule remains purely session-discipline; regressions possible when an agent spins up with stale context | Rejected per user's explicit directive to close all four gaps; advisory comment is the lowest-noise compromise |
| Implement as MCP server / custom bot | Rich context; structured feedback | Infrastructure debt, auth surface, dependency on external deployment | Rejected — GitHub Actions is the existing CI substrate and adds no new moving parts |
| Use `danger.js` or similar framework | Batteries-included review automation | Adds a JS / Ruby runtime dependency to CI purely for this check; vmaf CI is already C/Python/meson | Rejected — `grep` + shell in a standard `ubuntu-latest` runner is sufficient and auditable |

## Consequences

- **Positive**: the four process ADRs transition from discipline-only
  to discipline + automated safety net. Contributors get fast feedback
  on PR without waiting for reviewer attention. The blocking
  `deep-dive-checklist` catches the failure mode most likely to
  regress (missing `CHANGELOG.md` or `docs/rebase-notes.md` entries).
- **Positive**: the copyright pre-commit hook prevents a new `.c` or
  `.cu` file landing with the wrong header template — today this is
  caught only if a reviewer happens to open the file.
- **Negative**: every PR now runs three extra CI jobs (~10–20 s each
  on `ubuntu-latest`). Overhead is bounded; no matrix legs added.
- **Negative**: the `deep-dive-checklist` job becomes a required
  status check on `master` and must be kept passing; a broken workflow
  file blocks merges until fixed.
- **Neutral / follow-ups**: (a) existing pre-ADR-0108 PRs in-flight at
  the time of merge may need a courtesy comment-update to tick the
  checkboxes retroactively; (b) if the advisory `adr-backfill-check`
  produces excessive noise after 4 weeks, its trigger paths should be
  narrowed rather than the job disabled.

## References

- Source: `req` (user: paraphrased — "close the enforcement gaps for
  all four rule-bearing ADRs; bundle into a single PR per the
  consolidated-refactor preference").
- Session-start ADR list:
  [`docs/adr/README.md`](README.md).
- PR template carrying the checklist:
  [`.github/PULL_REQUEST_TEMPLATE.md`](../../.github/PULL_REQUEST_TEMPLATE.md).
- Related ADRs:
  [ADR-0100](0100-project-wide-doc-substance-rule.md),
  [ADR-0105](0105-copyright-handling-dual-notice.md),
  [ADR-0106](0106-adr-maintenance-rule.md),
  [ADR-0108](0108-deep-dive-deliverables-rule.md),
  [ADR-0116](0116-ci-workflow-naming-convention.md).
