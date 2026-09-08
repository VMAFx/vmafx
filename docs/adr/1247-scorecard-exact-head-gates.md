# ADR-1247: Bind Scorecard gates to their measured source and scope

- **Status**: Accepted
- **Date**: 2026-09-08
- **Deciders**: lusoris
- **Tags**: ci, security, supply-chain
- **Supersedes**: [ADR-0263](0263-ossf-scorecard-policy.md)

## Context

[Principles](../principles.md#6-compliance-targets) requires a Scorecard score
of at least 8.5 on master, while ADR-0263 retained a 6.2 floor and a 7.0 target.
The existing publisher neither enforces that threshold nor binds its remote
HEAD scan to the workflow's commit. The public API's latest result may describe
a different commit. Its 2026-09-08 score of 8.8 excluded three inconclusive
checks; it did not establish successful pinning, branch protection or signing.

The pinned action 2d114668 uses Scorecard 5.5.0. On PR events it scans local
files, with `repo.commit = unknown`; on master it scans the remote default HEAD.
Scorecard's explicit commit option also restricts the available check set.
Requiring a full default-branch result for an unmerged PR would deadlock the
merge that makes the result possible. Publisher authenticity restrictions
prohibit arbitrary scripts in its analysis job.

## Decision

Keep the restricted public publisher and add a separate master gate over its
same-run JSON artifact: require the event repository and exact commit, pinned
tool identity, complete 18-check set, valid scores and an aggregate of at least
8.5. Require an independent PR gate over all 11 local file-based checks at the
exact PR head, with the same numeric floor and before/after source receipts;
label that aggregate as local-only, never the full repository score. Scanner
errors, missing or stale reports fail closed. Display every individual result.
The only unassessed exception is `Signed-Releases = -1` with the exact upstream
reason `no releases found`; it is excluded as upstream does and never described
as signed. Zero scores, including review history and badge status, remain zero
and contribute to the aggregate. A successful aggregate is not compliance with
every individual check or an earned Best Practices badge.

## Alternatives considered

| Option | Benefit | Limitation | Decision |
| --- | --- | --- | --- |
| Same-run master report plus exact-head PR file checks | Enforces measurable scopes without waiting for a future merge | Two scopes must remain explicit; repository history/settings cannot be fully pre-merge tested | Selected |
| Gate the latest public API score | Simple | Mutable, possibly stale head and potentially incomplete checks | Rejected |
| Require a full master report for the PR head | Appears uniform | Impossible before merge with the pinned upstream interfaces | Rejected |
| Accept all inconclusive checks | Avoids red runs | Parser/API failures silently remove risk from the denominator | Rejected |
| Fail no-release state as a signing defect | Uniform treatment of -1 | No release exists to inspect; confuses unavailable evidence with failed signing | Rejected; exact reason is displayed as unassessed |

## Consequences

- The superseded 6.2/7.0 thresholds and blanket accepted blockers no longer apply.
  CII registration, independent reviews and repository enforcement are concrete
  remediation work, not permanently waived checks.
- PR and master gates run without path exclusions. The required aggregator
  must reject an absent, skipped, neutral or failed applicable Scorecard gate.
  PRs never wait for the master publisher; master never accepts a PR-local score.
- Both scopes recompute the weighted score from the pinned upstream risk levels
  and compare the unrounded value with 8.5. The report's rounded score must agree.
- Master advancing during a remote scan can cause an exact-head mismatch. That
  run fails; use the matching newer run rather than relabelling a newer report.
- Ordinary ephemeral read-only GitHub tokens remain the default. Only the
  publisher has OIDC and SARIF write permissions. Remote repository settings
  and external badge attestations require their own verified changes.
- The action SHA pins its source metadata, whose upstream Docker image still
  uses the v2.4.4 tag. The gate checks reported Scorecard version and source
  commit; this does not make that transitive image digest-pinned.
- These tool/documentation changes require focused contracts and normal hooks,
  not a native rebuild. Hosted publication and live aggregate acceptance are
  verified after integration; local fixtures cannot establish either.

## References

- `req`: user said **"well fix"** after the concrete Scorecard and
  Best Practices gaps.
- [Scorecard 5.5 check definitions](https://github.com/ossf/scorecard/blob/c395761df6afe1a69e476bc60a013a94bcbc153f/docs/checks.md).
- [Exact action options and PR mode](https://github.com/ossf/scorecard-action/blob/2d1146689b8cda280b9bc96326124645441f03bc/options/options.go).
- [Exact action runner](https://github.com/ossf/scorecard-action/blob/2d1146689b8cda280b9bc96326124645441f03bc/internal/scorecard/scorecard.go).
- [Publisher restrictions](https://github.com/ossf/scorecard-action/blob/2d1146689b8cda280b9bc96326124645441f03bc/README.md#workflow-restrictions).
- [Weighted score](https://github.com/ossf/scorecard/blob/c395761df6afe1a69e476bc60a013a94bcbc153f/pkg/scorecard/scorecard_result.go).
- [Research-0053](../research/0053-ossf-scorecard-investigation.md).
