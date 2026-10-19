<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-2126: Declare the single-maintainer gaps of OpenSSF Scorecard's Code-Review and Branch-Protection checks as exceptions

- **Status**: Accepted
- **Date**: 2026-10-06
- **Deciders**: lusoris
- **Tags**: security, governance, ci, supply-chain

## Context

Two OpenSSF Scorecard checks report on the repository's review model and cannot be raised by a code change. Code-Review (code-scanning alert 1) reports 0 of 18 recent changesets approved by a human. Branch-Protection (alert 1054) reports administrators exempt from the rules, one required approval and no code-owner review. The project has one maintainer. Every change lands through the local merge train: it restacks, builds, runs the per-pull-request gates (deliverables, state rows, silent revert, `praetorctl audit`) and lands only with the required status checks green on the exact head. A second approver does not exist, and requiring one would stop every merge.

The standing rule for a finding that the code cannot fix is a declared exception in the project's list (`.config/lint-exceptions.d/`, [`scripts/ci/lint_exceptions.py`](../../scripts/ci/lint_exceptions.py)): one file, one rule, a reason and an expiry, never a tier.

## Decision

Add two entries to the declared exception list, one rule each: `scorecard-code-review` (path `.github/CODEOWNERS`) and `scorecard-branch-protection` (path `docs/adr/0037-master-branch-protection.md`). Reason: single maintainer, the merge train's gates and required checks stand in for human review. Expiry: 2027-03-31, the nearest date inside the list's 400-day limit that covers `v1.0.0`; it ends earlier in practice when `v1.0.0` ships or a second maintainer joins, and the entries are then removed or renewed with the reason still true. The two code-scanning alerts are dismissed as "won't fix" with a comment that cites the entry. No ruleset or branch-protection setting changes.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Require a second approval and drop the admin exemption | Raises both scores | Blocks every merge of a one-person project; the train cannot satisfy it | The control would not be met by anyone |
| Leave the alerts open and unexplained | No policy text | Re-found on every audit; reads as forgotten | The count stays and the reason is lost |
| Dismiss without a list entry | Quick | A dismissal with no expiry is a tier (the exceptions rule) | Unbounded |
| Declared exceptions (chosen) | One list, one expiry, auditable, `check` fails when expired | The dismissal needs a renewal at expiry | |

## Consequences

- **Positive**: the open-alert count is explained by a bounded, checked entry; nothing in the repository's protection changes.
- **Negative**: Scorecard's aggregate score stays lower on these two checks until a second maintainer exists.
- **Neutral / follow-ups**: `python3 scripts/ci/lint_exceptions.py check` fails on the entries after 2027-03-31; the entries and the alert dismissals are reviewed at `v1.0.0` and when a second maintainer joins.

## References

- Q-045 (popup answer, 2026-10-06): "Declared exception (Recommended)" for Scorecard CodeReviewID and BranchProtectionID.
- [ADR-0037](0037-master-branch-protection.md): the protection settings. [ADR-1142](1142-whole-codebase-standards.md): standards bind every file, exceptions are declared.
