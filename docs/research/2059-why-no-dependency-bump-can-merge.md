<!-- markdownlint-disable MD013 -->

# 2059 — Why no dependency bump can merge

**Date**: 2026-09-15
**Scope**: the twenty open Renovate pull requests, two of them security fixes.
**Outcome**: a two-key configuration fix
([ADR-1251](../adr/1251-renovate-draft-automerge-deadlock.md)), plus a tooling
trap worth remembering when validating `renovate.json`.

## The observation

No Renovate pull request had merged since 2026-09-10. Twenty were open, all but
one of them drafts, including #1428 (`accelerate`) and #1429
(`pytorch-lightning`), both carrying security fixes. Six `packageRules` in
`renovate.json` declare `"automerge": true`, so at least the GitHub Actions,
pre-commit, Python-patch, Go, Cargo and Docker-digest groups should have been
landing unattended.

## Ruling out the obvious causes

**Renovate is not failing to run.** It had opened PRs as recently as the day
before, and `dependencyDashboard` was enabled by PR #1411, so its own errors
would surface as an issue. There is none.

**The bumps are not failing CI.** #1428 sits at 58 passing, 0 failing.

**`platformAutomerge` is not off.** It is `true` at the top level.

## The actual mechanism

Two changes, each correct on its own, compose into a deadlock.

[ADR-0679](../adr/0679-ci-draft-automerge-gate.md) makes `Required Checks
Aggregator` run on draft pull requests and **fail explicitly**, rather than
skipping. It is the single required branch-protection context. That was
deliberate: it closed a race where draft-era skipped check runs on the same head
SHA let a PR merge while the real ready-for-review CI was still queued. The
consequence, which is the point of the gate, is that **a draft can never satisfy
branch protection**.

PR #1411 then set `"draftPR": true` globally. Its commit message states the
reasoning: "The queue admits ONE non-draft PR at a time, and three Renovate PRs
sat in it today ahead of real work. draftPR is now true, so they queue as drafts
and the train promotes them in turn."

**There is no train.** Searching `.github/workflows/` and `scripts/` for
anything that marks a pull request ready for review returns nothing: no
`gh pr ready` call, no `markPullRequestReadyForReview` GraphQL mutation. Every
match for `ready_for_review` in the workflow tree is a `types:` trigger — a
consumer of the event, not a producer. The only promotion mechanism is a
maintainer typing the command.

So the chain is: Renovate opens a draft, the aggregator fails it by design,
`automerge` has a red required context and never fires, and nothing ever
un-drafts it.

`vulnerabilityAlerts` is the sharpest case. It sets `automerge: false`
deliberately, because a security bump should get human eyes. But it inherited
`draftPR: true`, so a security fix opened as a draft with a failing required
check and no signal that anything was wrong.

## Why the fix goes in `packageRules` and not at the top level

`draftPR` has no `parents` restriction in Renovate's option table
(`lib/config/options/index.ts`), exactly like `automerge`, which this repository
already uses inside `packageRules`. So `draftPR` is valid per rule, and the
global default can stay `true` for everything that still needs review.

## The tooling trap

`npx --yes --package renovate -- renovate-config-validator renovate.json`
reported **17 configuration errors** on the unmodified file, all of the form
`Invalid configuration option: customManagers[N].managerFilePatterns` plus
`kubernetes.managerFilePatterns`.

None of them is real. The stale `npx` cache resolved Renovate **37.440.7**,
which predates the `fileMatch` → `managerFilePatterns` rename;
`managerFilePatterns` is present in current Renovate with
`parents: [...AllManagersListLiteral, 'customManagers']`. npm's `latest` at the
time of writing is 44.93.4, and pinning it gives
`INFO: Config validated successfully against 1 file(s)`.

The error count was identical before and after the change, which is what made
the false positive safe to identify: a diff of validator output, not its
absolute result, is the reliable signal when the validator version is uncertain.

**Pin the version when validating this file:**

```bash
npx --yes --package renovate@44.93.4 -- renovate-config-validator renovate.json
```

## Follow-ups

The twenty already-open drafts are unaffected by a configuration change —
Renovate applies `draftPR` at creation time and does not un-draft an existing
pull request. They need promoting once, by hand.

A promotion workflow would still be worth having for the review-needed drafts
that legitimately stay drafts, which is the throughput problem PR #1411's
comment assumed was already solved. It would not help `vulnerabilityAlerts`,
where the requirement is immediate visibility rather than queueing.
