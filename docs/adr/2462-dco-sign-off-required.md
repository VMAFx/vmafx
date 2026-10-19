<!-- markdownlint-disable MD013 MD060 -->
# ADR-2462: Every commit carries a DCO sign-off, checked on pull requests

- **Status**: Accepted
- **Date**: 2026-10-08
- **Deciders**: lusoris
- **Tags**: `process`, `ci`, `license`

## Context

The project has no contributor licence agreement. Inbound contributions are
licensed under the licence of the file they touch (EUPL-1.2 for fork-authored
files, the inherited licence for files that carry other people's code;
[ADR-1250](1250-eupl-fork-relicense.md)). Until now nothing recorded that a
contributor had the right to submit what they submitted, and no check asked.
A Developer Certificate of Origin sign-off is the lightest mechanism that does:
a `Signed-off-by:` trailer on each commit, written by `git commit -s`.

The maintainer decided to adopt it (Q-190) and to enforce it with a CI check
(Q-192). Three details need a rule: which commits the check judges, who is
exempt, and how automation signs off.

## Decision

1. Every non-merge commit of a pull request carries a `Signed-off-by: Name
   <email>` trailer in the last block of its message, and the address is the
   commit's author or committer address. `CONTRIBUTING.md` and
   `docs/development/dco.md` describe the certificate (Developer Certificate of
   Origin 1.1) and how to sign off.
2. The required check **DCO Sign-off** (`rule-enforcement.yml`, listed in the
   required aggregator) runs `scripts/ci/check-dco.py` on the commits between
   the live target tip and the head. The range cannot be read: exit 2, never a
   pass.
3. Exemptions are explicit and narrow: a commit by an allow-listed bot
   (`renovate[bot]`, `dependabot[bot]`, `github-actions[bot]`) inside a pull
   request GitHub reports as opened by that bot, and the machine-generated
   release pull request (`scripts/ci/release-pr-exempt.sh`). A human commit on
   a bot branch, and a bot-looking author in a person's pull request, are not
   exempt.
4. Renovate signs off its own commits with the `:gitSignOff` preset
   (`commitTrailers`, documented in the Renovate configuration options), so the
   bot exemption is a second line.
5. Rollout (Q-201): the check applies to pull requests created at or after a
   cutoff timestamp kept in one file, `scripts/ci/dco-cutoff.txt`, and stated
   in `CONTRIBUTING.md`; older pull requests are grandfathered. A missing creation time is
   enforced, an unreadable cutoff exits 2.
6. The check is proven by tests that plant an unsigned commit, a sign-off for
   somebody else and a spoofed bot author, and each fails (the unsigned case
   was also run against a mutated gate that never rejects, which failed eight
   cases).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep the implicit inbound = outbound rule, no sign-off | Nothing to do | No record that the contributor had the right to submit; no check | The gap this ADR closes |
| Contributor licence agreement | Strongest legal record | A signing flow and an administrator; discourages small contributions | Heavier than the project wants (Q-190) |
| The hosted DCO GitHub App | No code to maintain | A third-party app with repository access; its exemptions are not ours; one more required context outside our workflows | The workflow check is ours, tested and replayable locally |
| Sign-off checked on the squashed commit on `master` only | One commit to check | Too late: the contributor has gone; the pull request is already merged | Checking the pull request's commits tells the author while they can fix it |
| Exempt every `[bot]` author | Simple | A person can name a commit author anything | The exemption needs the pull request author type as GitHub reports it |

## Consequences

- **Positive**: a recorded certificate per commit; the same check runs locally.
- **Negative**: every contributor, human or automated, must sign off; an
  unsigned branch needs `git rebase --signoff` and a force-push. Agents and the
  merge train commit with `-s` and keep the trailers when squashing.
- **Neutral / follow-ups**: the pull request template gains a checkbox;
  `master` history before this ADR stays unsigned and is not rewritten.

## References

- `Q-190` (maintainer decision, 2026-10-08): adopt the DCO.
- `Q-201` (maintainer decision, 2026-10-08): the check applies to pull
  requests opened after it lands; older ones are grandfathered.
- `Q-192` (maintainer decision, 2026-10-08): enforce it with a CI check on pull
  request commits, bot authors exempt.
- [ADR-1250](1250-eupl-fork-relicense.md) (licences of contributions),
  [ADR-0108](0108-deep-dive-deliverables-rule.md) (a pull request is judged by
  its template), [ADR-1284](1284-silent-revert-detection-gate.md) (the live-tip range),
  [ADR-1388](1388-release-pat-mode-gate-exemption.md) (release pull request
  exemption), `scripts/ci/release-pr-exempt.sh`.
