<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3273: Bypass mode `always` is a dated exception to the solo-repository standard

- **Status**: Superseded by [ADR-3313](3313-solo-ruleset-zero-reviews.md)
- **Date**: 2026-10-12
- **Deciders**: lusoris
- **Tags**: security, governance, ci

## Context

[ADR-1252](1252-solo-maintainer-declared-bypass.md) gave the master ruleset one bypass actor, the maintainer's account, at bypass mode `always`. The fleet's standard for a single-maintainer repository is the same bypass at mode `pull_request`: the maintainer may merge a pull request without an approval, and may not push to the default branch outside one.

The repository cannot meet that today. The merge train lands by direct push: it stacks the squash commits of the pull requests it accepted and adds a render commit (changelog, ADR index, rebase notes) that no pull request owns ([ADR-2197](2197-render-generated-docs-at-landing.md)). The ruleset keeps one required approval on purpose (ADR-1252, [ADR-1504](1504-decline-praetor-branch-ruleset.md)). With mode `pull_request` the forge would refuse every landing.

Until now mode `always` was a plain value of `.github/repository-security-policy.json`. Nothing marked it as a deviation, nothing gave it an end date, and the drift checker accepted it for as long as the live ruleset agreed.

## Decision

Mode `always` stays, as a declared and dated exception. `.config/lint-exceptions.d/solo-repository-bypass-mode.toml` holds one entry for `.github/repository-security-policy.json` with expiry 2027-01-09.

`scripts/dev/check_repository_security.py` binds the two files. A declared bypass mode other than `pull_request` without an unexpired entry of that rule for the policy file is a finding. An entry that remains after every declared mode is `pull_request` is a finding too. `python3 scripts/ci/lint_exceptions.py check` reports the entry once it is past its date, as it does for every entry of the list.

The guard for the time in between is the train itself: it refuses to land a head whose required contexts are not green.

Pull-request mode follows as its own unit of work: the train merges through the forge API, the render commit lands through its own pull request, then the policy file declares `pull_request`, the entry is removed and an ADR supersedes this one.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Declare `pull_request` now | Meets the standard at once | The forge refuses the train's push; nothing lands until the train is rebuilt | Stops every landing |
| Drop the approval requirement, then remove the bypass | No bypass actor at all | Changes the review policy ADR-1252 and ADR-1504 keep on purpose | A different decision, not part of this one |
| Keep `always` undeclared | No change | A permanent deviation with no owner and no end date | An exception without an expiry is a tier |
| Dated exception with a binding check (chosen) | The deviation is visible, bounded and fails closed at its date | A renewal or the train change is due by 2027-01-09 | |

## Consequences

- **Positive**: the deviation from the standard has one declared place, a reason and an end date, and the security checker fails without it.
- **Negative**: until the train merges through the forge, the maintainer's account can still push to `master` outside a pull request.
- **Neutral / follow-ups**: the train change (merge through the API, render commit through a pull request) is due before 2027-01-09; the entry is renewed only while the train still lands by push.

## References

- Controller ledger `vmafx-train-bypass-pr-mode` (2026-10-11, bypass mode of the solo repository; paraphrased): keep mode `always` as a declared, dated exception with expiry 2027-01-09; pull-request mode becomes its own unit once the train merges through the forge.
- [ADR-1252](1252-solo-maintainer-declared-bypass.md): the single bypass actor. [ADR-1504](1504-decline-praetor-branch-ruleset.md): the ruleset the repository keeps. [ADR-1142](1142-whole-codebase-standards.md): exceptions are declared, never tiers.
