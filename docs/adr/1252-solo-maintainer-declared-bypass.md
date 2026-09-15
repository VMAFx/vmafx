<!-- markdownlint-disable MD060 -->
# ADR-1252: A single-maintainer repository declares its bypass actor instead of pretending to have a reviewer

- **Status**: Accepted, Supersedes [ADR-1248](1248-repository-security-enforcement.md)
- **Date**: 2026-09-15
- **Deciders**: Lusoris
- **Tags**: security, ci, governance, adr, fork-local

## Context

[ADR-1248](1248-repository-security-enforcement.md) created ruleset
`VMAFx master security` (id 22587111) on 2026-09-08 with no bypass actors and
one required independent human approval. It rejected administrator bypass
explicitly, and its Consequences accepted that "the merge train remains paused"
as a result.

Seven days of evidence say the control cannot be satisfied as written.

`VMAFx` is an organisation with exactly one collaborator, `lusoris`, who is the
author of every open pull request. GitHub does not allow a pull request author to
approve their own pull request, and the ruleset listed no bypass actor. So the
required approval had no one who could give it.

The measured effect: the last merge in the repository was #1413 at 2026-09-08
16:15 UTC, and the ruleset was created at 23:26 local the same day. Between then
and 2026-09-15, **nothing merged at all** — not #1396, which was fully green at
70 passing checks and carried the container fixes that `master`'s own
`Docker Image Build` and `FFmpeg SYCL` lanes needed; not the two security
dependency bumps #1428 and #1429; not any of the twenty-odd correctness fixes on
the pre-rc.1 queue. Every merge in the repository's history happened before the
ruleset existed, each with zero reviews, which is the same thing said a different
way: the control has never once been satisfied, only avoided.

A control that cannot be satisfied is not a control. It is an outage that looks
like a policy, and the failure mode is worse than a declared exception: the work
does not stop, it moves to administrator overrides and unreviewed direct pushes,
which is precisely the behaviour ADR-1248 set out to prevent, now with no record
of when it happened.

## Decision

Keep every part of ADR-1248 except the empty bypass list.

The ruleset keeps `enforcement: active`, master-only scope, one required
approving review, `dismiss_stale_reviews_on_push`, `require_last_push_approval`,
`required_review_thread_resolution`, strict up-to-date required status checks
bound to `Required Checks Aggregator`, linear history, and blocked deletion and
force pushes.

It gains **exactly one** bypass actor, declared in
`.github/repository-security-policy.json`:

```json
{"actor_id": 6750258, "actor_type": "User", "bypass_mode": "always"}
```

That is `lusoris`, by numeric user id, not a role tier. A `RepositoryRole`
entry would have granted bypass to everyone who ever holds that role; a `User`
entry names one account and nothing else.

`scripts/dev/check_repository_security.py` changes from "the ruleset must have no
bypass actors" to "the ruleset's bypass actors must be exactly the declared
list". The declared exception is therefore verified on every Scorecard master
run, and an **undeclared** actor — including a second one added beside the
declared one, or the same actor widened from `always` to `exempt` — still fails
the gate.

When the repository has a second maintainer who can review, this ADR should be
superseded in turn and the bypass actor removed. That is a capacity problem, not
a policy preference.

## Alternatives considered

| Option | Benefits | Costs | Decision |
| --- | --- | --- | --- |
| Declare one `User` bypass actor and verify it | The exception is in the repository, reviewable, and gate-enforced; work proceeds | Weaker than two-party review; a non-admin reader can verify only the count, not the identity | **Selected** |
| Keep ADR-1248 unchanged | Strongest stated control | The control is unsatisfiable with one maintainer, so it is enforced by nobody and routed around by administrator overrides. Seven days and zero merges is the measurement | Rejected |
| Drop `required_approving_review_count` to 0 | Keeps the no-bypass assertion literally true, needs no checker change | Removes the review requirement for any future contributor too, and records nothing about why | Rejected |
| Add a second account to approve | Preserves two-party review on paper | It would be the same person behind both accounts. A rubber stamp that looks like independent review is worse than a declared exception | Rejected |
| Grant bypass to the `RepositoryRole` admin tier | Simpler payload | Grants bypass to a role rather than a person, so it silently widens as soon as anyone else is made an admin | Rejected |

## Consequences

Merges no longer require an independent human review, because there is no second
human. The requirement stays on the ruleset, so it applies the moment a second
reviewer exists and the bypass is removed.

`Required Checks Aggregator` remains a required status check and is **not**
bypassed in practice: CI still has to be green. The bypass removes the approval
requirement, not the test requirement.

A reader without ruleset write access can confirm only **how many** actors
bypass, because REST hides the actor list from them and the GraphQL query
exposes `bypassActors.totalCount` alone. Such a reader cannot distinguish the
declared actor from a substituted one with the same count. A reader with admin
rights compares identities exactly, and the Scorecard master run does. This is a
real reduction in what an outside observer can verify, and it is the price of
declaring the exception rather than hiding it.

Scorecard's Branch-Protection score will fall, since it reads administrator
bypass as a weakness. That is an accurate reading of the trade-off and should not
be worked around by making the repository look stricter than it is.

## References

- req: user direction, 2026-09-15 — paraphrased: this is currently a
  single-worker repository, so keep the bypass, and file the missing
  single-maintainer work mode upstream against praetor.
- [ADR-1248](1248-repository-security-enforcement.md) — superseded by this ADR.
- [ADR-0679](0679-ci-draft-automerge-gate.md) — the required aggregator is the
  control that still binds.
- [Research 2057](../research/2057-repository-security-enforcement.md) — the
  original enforcement measurements, with this ADR's addendum.
