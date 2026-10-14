<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3313: The solo master ruleset asks for no approval and bypasses at pull-request mode

- **Status**: Accepted
- **Date**: 2026-10-14
- **Deciders**: lusoris
- **Tags**: security, governance, ci

## Context

[ADR-3273](3273-solo-bypass-mode-dated-exception.md) kept the maintainer's ruleset bypass at mode `always` as a dated exception, because the merge train landed by direct push and the ruleset required one approving review ([ADR-1252](1252-solo-maintainer-declared-bypass.md), [ADR-1504](1504-decline-praetor-branch-ruleset.md)). The repository has one maintainer: the required approval could only be met through the bypass, so every pull request reported `BLOCKED` and the fleet rule that merges happen only at a clean merge state could not be met.

On 2026-10-13 the live ruleset 22587111 was changed: no required approval, no last-push approval, bypass mode `pull_request`; every other rule stayed. The tracked `.github/repository-security-policy.json` still declared one approval, last-push approval and mode `always`, so `scripts/dev/check_repository_security.py` failed in the Scorecard Master Gate on master. Generated files now land through a render pull request ([ADR-3295](3295-render-only-pull-request.md)), so nothing needs a direct push any more.

## Decision

The policy file declares the live ruleset field for field: `required_approving_review_count` 0, `require_last_push_approval` false, `require_code_owner_review` false, the remaining pull-request parameters the forge reports (`required_reviewers` empty, `dismissal_restriction` disabled, `require_extra_approval_for_unattributed_changes` true, `dismiss_stale_reviews_on_push` true, `required_review_thread_resolution` true, squash and rebase merges), and one bypass actor, the maintainer's account, at mode `pull_request`.

The exception entry `.config/lint-exceptions.d/solo-repository-bypass-mode.toml` is removed: the mode it excused is gone. The check of ADR-3273 stays in `main()`: another bypass mode without an unexpired entry of rule `solo-repository-bypass-mode` fails, and so does an entry left behind while every mode is `pull_request`.

This supersedes ADR-3273 and amends ADR-1252 and ADR-1504 in one point: the master ruleset no longer requires an approving review. Required status checks, linear history, signed commits, thread resolution and the refusal of force-push and deletion are unchanged. The bypass is removed as soon as a second maintainer can review.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep one required approval, bypass at `pull_request` | Review count unchanged on paper | No second person can approve; every pull request stays `BLOCKED` and merges need the bypass | The approval is met by nobody; the clean-merge rule is never met |
| Keep the policy file at one approval and `always` | No file change | The drift check fails on every master run and blocks the Scorecard Master Gate | Declares a ruleset that no longer exists |
| Weaken the drift check to ignore review fields | Gate green at once | The check stops catching a real review-policy change | A check that does not compare is not a check |
| Declare the live ruleset, drop the exception (chosen) | Policy file and forge agree; merges happen at a clean state; no standing exception | No human approval on master until a second maintainer exists | |

## Consequences

- **Positive**: the policy file equals the live ruleset, the security checker passes on master, pull requests reach a clean merge state once their required checks pass, and the maintainer cannot push to `master` outside a pull request.
- **Negative**: no approving review is required; the required checks and the gates are the only barrier before a merge.
- **Neutral / follow-ups**: the pull requests merged with the bypass before this rule took effect stay as they are. A second maintainer brings back a required approval in a new ADR.

## References

- Controller ledger `vmafx-ruleset-review-count` (2026-10-13; paraphrased): set ruleset 22587111 to no required approval, no last-push approval and bypass mode `pull_request`, keep every other rule, read it back.
- Controller order `vmafx-red-main-20261013-scorecard` (2026-10-13; paraphrased): align the policy file to the live ruleset, every field including the bypass mode, and show the checker refusing a copy with one required approval.
- Fleet rule for single-maintainer repositories (paraphrased): zero approvals, signed commits, the maintainer's bypass at pull-request mode.
- [ADR-3273](3273-solo-bypass-mode-dated-exception.md) (superseded), [ADR-1252](1252-solo-maintainer-declared-bypass.md), [ADR-1504](1504-decline-praetor-branch-ruleset.md), [ADR-3295](3295-render-only-pull-request.md).
