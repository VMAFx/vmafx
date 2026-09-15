<!-- markdownlint-disable MD060 -->
# ADR-1251: Renovate opens automerge-eligible and security bumps ready for review

- **Status**: Accepted
- **Date**: 2026-09-15
- **Deciders**: Lusoris
- **Tags**: ci, dependencies, renovate, merge-train, adr, fork-local

## Context

No dependency bump has landed since 2026-09-10. Twenty Renovate pull requests
are open, two of them carrying security fixes (#1428 `accelerate`, #1429
`pytorch-lightning`), and none of them can merge. The cause is an interaction
between two changes that are each correct on their own.

[ADR-0679](0679-ci-draft-automerge-gate.md) makes `Required Checks Aggregator`
run on draft pull requests and **fail explicitly**. It is the single required
branch-protection context, so a draft can never satisfy branch protection. That
is deliberate: it closed a race where draft-era skipped check runs let a PR
merge while real CI was still queued.

PR #1411 then set `"draftPR": true` globally in `renovate.json`, so that bot
PRs queue as drafts instead of occupying the ready-for-review queue ahead of
human work. Its stated expectation was that "the train promotes them in turn".

There is no such train. Nothing in `.github/workflows/` marks a pull request
ready for review, and no script under `scripts/` does either — verified by
searching both trees for `ready_for_review` producers and for
`markPullRequestReadyForReview`. The only promotion mechanism is a maintainer
typing `gh pr ready`. So every Renovate PR opens as a draft, the aggregator
fails it by design, `automerge` never fires, and the PR sits.

The six `packageRules` that set `"automerge": true` — GitHub Actions minor and
patch, pre-commit hook revisions, Python patch, Go minor and patch, Cargo minor
and patch, and Docker digests — are exactly the updates the repository has
decided need no human review. Configuring them to open in a state where merging
is impossible cancels that decision silently. `vulnerabilityAlerts` is worse:
it sets `automerge: false` on purpose, but inheriting `draftPR: true` means a
security fix parks as a draft with a red required context and no notification.

## Decision

Keep `"draftPR": true` as the repository default, and set `"draftPR": false` on
the rules where a draft is unmergeable by construction:

- the six `packageRules` carrying `"automerge": true`, so the platform can
  actually merge what the repository has already agreed to merge unattended;
- `vulnerabilityAlerts`, so a security bump is visible and mergeable the moment
  it opens, even though it still requires a human to approve it.

Everything else — majors, minors outside the grouped ecosystems, anything with
`automerge: false` — keeps opening as a draft and stays out of the ready queue,
which is what #1411 wanted.

`draftPR` carries no `parents` restriction in Renovate's option table
(`lib/config/options/index.ts`), the same as `automerge`, so it is valid inside
`packageRules` and inside `vulnerabilityAlerts`.
`renovate-config-validator` from Renovate 44.93.4 reports
`Config validated successfully` for the amended file.

## Alternatives considered

**Drop `draftPR` entirely.** Restores the problem #1411 fixed: every bot PR,
including the majors that need a careful look, lands in the ready queue.

**Exempt Renovate from the ADR-0679 draft gate.** Re-opens the race the gate
exists to close, and does so for the one author that merges without review. The
gate is not the defect.

**Build the promotion automation the comment assumes.** A workflow that marks
bot drafts ready in turn would also work, and may still be worth having for
throughput. It is strictly more machinery than the two-line config fix needs,
and it would not help `vulnerabilityAlerts`, where the point is to surface the
PR immediately rather than to queue it.

**Leave it and merge bumps by hand.** This is the status quo. It produced a
five-day stall and two unmerged security fixes.

## Consequences

- Automerge-eligible bumps open ready for review and merge unattended once the
  aggregator is green, as the rules always intended.
- Security bumps open ready for review, labelled `security` and `dependencies`,
  and still wait for a human because `automerge` stays `false` there.
- The ready queue now admits bot PRs again, but only the subset that needs no
  review. `prConcurrentLimit: 3` and `branchConcurrentLimit: 5` from #1411 still
  bound how many can be in flight.
- The twenty already-open drafts are unaffected by a config change: Renovate
  does not un-draft an existing PR. They have to be promoted once by hand.

## References

- req: user direction, 2026-09-15 — paraphrased: the pending dependency version
  bumps also need dealing with.
- [ADR-0679](0679-ci-draft-automerge-gate.md) — the draft aggregator gate.
- PR #1411 — `fix(ci): stop Renovate opening duplicate PRs and jamming the
  merge window`.
- [Renovate `draftPR`](https://docs.renovatebot.com/configuration-options/#draftpr).
- [Research 2059](../research/2059-why-no-dependency-bump-can-merge.md) —
  the diagnosis and the validator-version trap.
