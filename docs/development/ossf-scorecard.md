# OSSF Scorecard Policy

[ADR-0263](../adr/0263-ossf-scorecard-policy.md) sets the fork's
policy on the OSSF Scorecard supply-chain audit: which checks are
gated CI failures vs informational, the documented exception list
(e.g. signed-releases requirements that interact with the fork's
Sigstore-keyless flow per
[ADR-0010](../adr/0010-sigstore-keyless-signing.md)), and the
quarterly cadence for re-evaluating exceptions.

The Scorecard workflow ships at
`.github/workflows/scorecards.yml`; the published score is
visible on the project's GitHub Insights page.

## Policy

- OSSF Scorecard runs as a scheduled supply-chain audit rather than as
  a per-commit correctness test.
- Checks that reflect actionable repository hygiene are treated as
  required maintenance work.
- Checks that conflict with the fork's documented release architecture
  are tracked as explicit exceptions in ADR-0263 rather than silently
  ignored.
- Exception reviews happen quarterly or when Scorecard changes a check's
  semantics.

## Current Exceptions

The most important exception is release signing. The fork uses Sigstore
keyless signing through GitHub OIDC per
[ADR-0010](../adr/0010-sigstore-keyless-signing.md). Any Scorecard
finding that assumes long-lived signing keys must be interpreted through
that keyless flow instead of treated as a missing control.

## Operator Workflow

1. Open the scheduled Scorecard run in GitHub Actions.
2. Compare any score drop against ADR-0263's exception list.
3. File or update a state row when the drop is actionable.
4. Update ADR-0263 only when a check's expected disposition changes.

## See also

- [`docs/development/release.md`](release.md) — release pipeline
  including signing prerequisites.
- [ADR-0010](../adr/0010-sigstore-keyless-signing.md) — Sigstore
  signing flow that the Scorecard "signed-releases" check
  validates.
- [ADR-0263](../adr/0263-ossf-scorecard-policy.md) — design
  decision.
