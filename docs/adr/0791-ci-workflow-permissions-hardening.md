# ADR-0791: CI workflow permissions hardening — explicit read-only defaults on all workflows

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `ci`, `security`

## Context

A security audit of `.github/workflows/` found that two workflows — `go-ci.yml` and
`rust-ci.yml` — lacked a top-level `permissions:` block. GitHub's default token
permissions when no explicit block is present depend on the repository's organization
settings and can be as broad as `write-all`. All other 22 workflows in the repo already
carried an explicit top-level `permissions: contents: read` (or `read-all`) block,
limiting the GITHUB_TOKEN to the minimum required by each job.

All third-party actions were already pinned to 40-character SHA digests (zero unpinned
tags). No `pull_request_target` triggers were found. Secrets access was limited to
`GITHUB_TOKEN` only, scoped to jobs that genuinely need it (push to GHCR, create issues,
upload SARIF). Write permissions (`packages`, `id-token`, `security-events`, etc.) are
already declared per-job rather than at workflow level. OIDC (`id-token: write`) is used
exclusively for Sigstore keyless signing and Trusted Publishing — both legitimate and
properly scoped. Untrusted event data (`PR_TITLE`, `PR_BODY`, `HEAD_REF`) is passed via
environment variables, not inline `${{ }}` interpolation inside `run:` scripts, preventing
script injection.

The only remediation needed was adding `permissions: contents: read` to the two workflows
that had none.

## Decision

Add `permissions:\n  contents: read` at workflow level to `go-ci.yml` and `rust-ci.yml`,
matching the pattern already in place for every other workflow in the repository.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Rely on org default settings | Zero code change | Default may be `write-all`; brittle across org changes | Non-deterministic; violates least-privilege |
| Per-job permissions only | Fine-grained | Verbose; no safety net if a new job is added without a per-job block | Workflow-level read default is a better safety net |

## Consequences

- **Positive**: all 24 workflows now have an explicit top-level permissions block;
  `go-ci` and `rust-ci` GITHUB_TOKEN is locked to `contents: read` regardless of
  org defaults. The GITHUB_TOKEN cannot be used to push code or create releases from
  those jobs even if a compromised action attempts it.
- **Negative**: none — `go vet`, `go test`, `cargo fmt/clippy/test` do not require
  any write permission.
- **Neutral**: ongoing policy — any new workflow file must include a top-level
  `permissions:` block before merging.

## References

- GitHub docs: [Workflow permissions](https://docs.github.com/en/actions/security-guides/automatic-token-authentication#permissions-for-the-github_token)
- GitHub hardening guide: [Security hardening for GitHub Actions](https://docs.github.com/en/actions/security-guides/security-hardening-for-github-actions)
- Related PR: this change.
