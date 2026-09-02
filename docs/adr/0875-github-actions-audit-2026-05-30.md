<!-- markdownlint-disable MD060 -->
# ADR-0875: GitHub Actions hardening audit (2026-05-30)

- **Status**: Accepted
- **Date**: 2026-05-30
- **Deciders**: lusoris, Claude
- **Tags**: `security`, `ci`, `supply-chain`

## Context

The fork's GitHub Actions tree has grown to 24 workflows. Periodic audits
keep the supply-chain posture from drifting. This pass checked three
properties recommended by OpenSSF Scorecard and the GitHub Actions
security guide:

1. Every third-party action pinned to a 40-character commit SHA (not a
   tag or branch). Tags are mutable; a compromised maintainer can
   retroactively repoint a tag at malicious code, which then runs in
   every workflow that names that tag.
2. A least-privilege top-level `permissions:` block on every workflow,
   so jobs that do not declare their own permissions inherit `contents:
   read` rather than the repo's default-write token.
3. `persist-credentials: false` on every `actions/checkout` step where
   the job does not push back to git, open issues/PRs, or attach to a
   release. The default behaviour writes `GITHUB_TOKEN` into the
   runner's `.git/config`, exposing it to any later step.

The baseline turned out to be in very good shape — every `uses:` in the
22 workflows audited was already SHA-pinned, and 22 of 24 workflows
had a top-level `permissions:` block. Three gaps remained:

- `go-ci.yml` and `rust-ci.yml` had no top-level `permissions:`, so
  every job ran with the repo's default token scopes.
- Five `actions/checkout` steps (two in `sanitizers.yml`, three in
  `supply-chain.yml`) lacked `persist-credentials: false`. The
  supply-chain ones were carried under an older blanket whitelist that
  no longer matches actual usage — only the `sign`, `slsa-provenance`,
  `mcp-sign`, `mcp-publish-pypi`, and `attach-to-release` jobs in
  `supply-chain.yml` need a non-default token, and those jobs run in
  their own steps with their own checkouts.

Two workflows (`lint-and-format.yml`, `libvmaf-build-matrix.yml`) are
in-flight under PR #342 and PR #325 and were deliberately skipped to
avoid merge conflicts; they will be re-audited after those PRs land.

## Decision

Adopt a uniform baseline for the fork's GitHub Actions tree:

1. Every workflow declares a top-level `permissions:` block. The
   default is `contents: read`; jobs widen explicitly when they need
   to publish artefacts, sign with OIDC, push to the repo, or write to
   the issue tracker.
2. Every `actions/checkout` step in jobs that do not push to git,
   open issues/PRs, or attach release assets sets
   `persist-credentials: false`. The whitelist of token-persisting
   checkouts is documented per job, not per workflow.
3. Every `uses:` is pinned to a 40-character SHA with a trailing
   `# vX.Y.Z` comment for human readability. (Already true today;
   this ADR codifies the rule going forward.)

This pass applies the rule to the four workflows missing pieces of the
baseline (`go-ci.yml`, `rust-ci.yml`, `sanitizers.yml`,
`supply-chain.yml`).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Adopt the rule and fix every drift in one PR (chosen) | Single migration; the rule lands enforced everywhere | One audit PR; need to skip in-flight workflows | Chosen — cheap, immediate, low blast radius |
| Defer until the next PR happens to touch each workflow | Smallest single PR | Drift persists for weeks; new workflows added in the meantime can ship without the rule | Rejected — the gap remains exploitable in the interim |
| Enforce in CI via a Scorecard / actionlint policy gate, then backfix | Permanent backstop | Requires a separate ADR + gate wiring; doesn't fix today's gaps | Deferred — desirable as a follow-up, not a substitute for the backfix |

## Consequences

- **Positive**: every workflow now starts from `contents: read`; every
  checkout that does not need a token does not get one. Reduces blast
  radius if any third-party action in the dependency tree is later
  compromised.
- **Positive**: a future contributor adding a new workflow inherits the
  rule by example — every neighbouring file demonstrates the pattern.
- **Negative**: the audit takes ~30 minutes whenever someone adds a
  cluster of workflows; an actionlint or Scorecard policy gate would
  amortise that cost. Tracked as a follow-up.
- **Neutral**: the two skipped workflows (`lint-and-format.yml`,
  `libvmaf-build-matrix.yml`) will be re-audited after PR #342 and
  PR #325 merge.

## References

- OpenSSF Scorecard — Pinned-Dependencies check.
- GitHub docs — "Security hardening for GitHub Actions".
- Prior fork ADRs: [ADR-0379](0379-symbol-visibility.md) (supply
  chain), the earlier persist-credentials sweep at
  `changelog.d/security/actions-checkout-persist-credentials-false.md`.
- Source: `req` — user-dispatched audit task ("Audit GitHub Actions
  workflows for action-version pinning, permissions, and OIDC
  adoption").
