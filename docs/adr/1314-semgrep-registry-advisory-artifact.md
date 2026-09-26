<!-- markdownlint-disable MD013 MD060 -->

# ADR-1314: Keep unpinned Semgrep registry results advisory

- **Status**: Accepted
- **Date**: 2026-09-25
- **Deciders**: lusoris
- **Tags**: `ci`, `security`, `policy`, `supply-chain`

## Context

ADR-1297 made every check that reports a real pull-request verdict required,
including the `Semgrep OSS` check produced by GitHub Code Scanning. The
security workflow uploaded two SARIF categories with that tool identity:
repository-owned `.semgrep.yml` rules and three moving Semgrep Registry packs.
Consequently, the supposedly advisory registry scan could block every merge
after an upstream pack changed, even when this repository had not changed.
`continue-on-error` only made registry fetch failures non-fatal; it could not
make uploaded findings advisory.

The local rules are reviewed with this repository and use `--error`, so their
code-scanning verdict remains suitable for a required context. The registry
packs are intentionally unpinned discovery inputs and must not acquire merge
authority.

## Decision

We will upload only the repository-owned `semgrep-local` SARIF to GitHub Code
Scanning. The registry scan remains in the required Semgrep job with
`continue-on-error: true`, but its SARIF is retained for 14 days as an ordinary
workflow artifact. A source contract rejects any restoration of the
`semgrep-registry` code-scanning category while requiring the advisory artifact
and the required `Semgrep OSS` marker for local rules.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Upload both SARIF categories and accept registry findings as required | All findings appear in one Security-tab view | A moving, unauthenticated upstream ruleset can block merges without a repository diff | Violates the intended advisory boundary and reproducible-policy principle |
| Remove the registry scan | Simplest and cheapest workflow | Loses useful broad discovery coverage | Discovery remains valuable when it has no merge authority |
| Route registry results to a second GitHub code-scanning configuration | Keeps Security-tab presentation | SARIF tool/check identity and branch protection do not provide a reliable per-category advisory boundary here | More configuration without a mechanically provable separation |
| Archive registry SARIF as a workflow artifact | Preserves diagnostics and cleanly separates authority | Reviewers must download the artifact instead of using inline Security-tab annotations | Chosen: the separation is explicit, pinned, and testable |

## Consequences

- **Positive**: only version-controlled local rules can make `Semgrep OSS`
  block a merge; registry-pack churn cannot introduce an unreviewed gate.
- **Positive**: registry findings and scanner output remain downloadable for
  investigation for 14 days.
- **Negative**: registry findings no longer create inline GitHub Code Scanning
  annotations or automatic alert-lifecycle records.
- **Neutral / follow-ups**: local-rule findings, the Semgrep job, and the
  `Semgrep OSS` code-scanning check remain required and fail closed.

## References

- [ADR-1297](1297-ci-gate-every-reporting-check.md)
- [Research-1314](../research/1314-semgrep-registry-sarif-routing.md)
- [`T-SEMGREP-REGISTRY-PACKS-NOW-BLOCKING-2026-09-22`](../state.md)
- Source: `req` ("we fix everything until we cant find anything anymore for now")
