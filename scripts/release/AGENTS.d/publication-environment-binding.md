---
paths:
  - scripts/release/tests/test-publication-environment-binding.sh
  - .github/workflows/supply-chain.yml
  - .github/workflows/docker-publish-*.yml
invariant: Server-side environment preflight + YAML binding check; SHA-pinned actions; exact tag identity.
---
<!-- markdownlint-disable MD013 MD060 -->
# Publication environment binding and build provenance (ADR-1151, ADR-1356)

## Publication environment binding

Environment names must also *exist server-side with required
reviewer*. GitHub auto-creates referenced environment that does not
exist, with empty rule set — job naming missing environment runs
with no approval gate at all. `test-publication-environment-binding.sh`
only greps YAML, cannot see that; `supply-chain.yml`'s
`validate-release` carries live
`gh api repos/$GITHUB_REPOSITORY/environments/<name>` preflight, fails
closed (ADR-1151). Keep both — grep catches workflow that forgets
binding; preflight catches repository that never configured
environment.

Two Docker publishers bind every GHCR write/OIDC job to
`release-publish`; `supply-chain.yml` binds Sigstore and GitHub
Release writes there, keeping PyPI on its trusted-publisher
environment `pypi-publish`. Build-provenance jobs (`provenance`,
`mcp-provenance`, ADR-1356) are `release-publish` jobs with exactly
`attestations: write`, `id-token: write`, `contents: read`; they run
SHA-pinned `actions/attest-build-provenance` on build job `hashes`, verify
bundle with consumer `gh attestation verify` recipe, upload it as workflow
artifact only; protected attachment job publishes both bundles. Every
`uses:` in release workflows pinned to full commit SHA; no
`slsa-github-generator` (org `sha_pinning_required`). Container
verification accepts only exact release-tag workflow identity, never
`@.*` ref wildcard.

Test coverage:
`scripts/release/tests/test-publication-environment-binding.sh`.
