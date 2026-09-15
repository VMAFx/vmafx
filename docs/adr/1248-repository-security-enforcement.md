# ADR-1248: Enforce repository security through public rulesets

- **Status**: Superseded by [ADR-1252](1252-solo-maintainer-declared-bypass.md)
- **Date**: 2026-09-08
- **Deciders**: lusoris
- **Tags**: security, ci, governance

## Context

The live master protection required the aggregate CI check but allowed administrator
bypass, required no independent review, and accepted branches that were behind
master. Scorecard could not inspect that classic rule with its workflow token.
The documented private vulnerability reporting channel was also disabled.
GitHub repository rulesets expose protection to readers without a personal token.

## Decision

Keep the existing classic protection and add an active, master-only ruleset with
no bypass actors, one independent human approval, dismissal of stale reviews,
approval after the latest push, resolved review threads, strict up-to-date CI,
linear history, and blocked deletion/force pushes. Bind the existing Required
Checks Aggregator context to the verified GitHub Actions app. Store its exact
payload in the repository, enable private vulnerability reporting, and provide a
read-only drift check. Administrative changes use reviewed payloads and live
readback; the checker never changes remote settings.

## Alternatives considered

| Option | Benefits | Costs | Decision |
| --- | --- | --- | --- |
| Add a public ruleset and retain classic protection | Stronger enforcement with no migration gap or new stored token | Both policies remain visible | Selected |
| Strengthen only classic protection | Fewer policy objects | Scorecard still cannot inspect it with its normal token | Rejected |
| Replace classic protection immediately | One policy object | Unnecessary deletion and migration risk | Rejected |
| Require two approvals and code-owner approval | Higher Scorecard tier | A larger maintainer quorum than the minimum independent review control | Defer until reviewer capacity is established |
| Keep administrator bypass | Faster solo merges | Human review and CI enforcement can be bypassed | Rejected |

## Consequences

Merges require an independent human review even when the author is an administrator.
AI reviews and tests do not replace that person. The merge train remains paused
until its existing local and hosted gates pass; this policy cannot waive them.
Private reports can use the route already promised in SECURITY.md. The checked-in
configuration makes drift reviewable without granting CI administration rights.
The ruleset applies only to master; it does not add release/tag policy or alter
other repository rules. Scorecard may need its next successful scan to reflect
the new settings. The assessment of past reviews remains historical.

## References

- `req`: user asked whether Scorecard documentation was followed, then said
  "well fix" after the missing review, administrator and freshness controls
  were identified.
- [GitHub ruleset API](https://docs.github.com/en/rest/repos/rules#create-a-repository-ruleset).
- [Private vulnerability reporting API](https://docs.github.com/en/rest/repos/repos#enable-private-vulnerability-reporting-for-a-repository).
- [Scorecard action authentication](https://github.com/ossf/scorecard-action/tree/2d1146689b8cda280b9bc96326124645441f03bc#authentication-with-fine-grained-pat-optional).
- Exact Scorecard v5.5.0 branches handler, commit
  `c395761df6afe1a69e476bc60a013a94bcbc153f`, ignores classic-rule permission errors
  when ruleset data is available.
