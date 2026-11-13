---
paths:
  - .github/workflows/docker-publish-production.yml
  - .github/workflows/docker-publish-operator-node.yml
  - .github/workflows/supply-chain.yml
invariant: Every container image dual-signed with cosign and attest-build-provenance; SBOM attached; smoke verifies cosign.
---
# Signing + attestation chain invariants (ADR-0902)

Container builds in `.github/workflows/docker-publish-production.yml` and
`.github/workflows/docker-publish-operator-node.yml`, plus release-blob signing
in `.github/workflows/supply-chain.yml`, carry multi-layer signing chain
load-bearing for
[release.md](../../docs/development/release.md) consumer verification recipes.

- Every container build job (CPU, CUDA, ROCm, oneAPI, Python MCP server, Go
  scoring server, operator, node)
  must run
  **both** `cosign sign --yes` **and** `actions/attest-build-provenance@<v4>`
  against same `${{ steps.push.outputs.digest }}`. Two
  attestations cover different consumer toolchains (cosign for
  Sigstore-native consumers, `gh attestation verify` for GitHub-native
  consumers); neither replaces other. Removing either side is
  policy change, needs superseding ADR.
- Both Docker workflows start with tag-bound validation job. Release or
  manual recovery must identify same published, non-prerelease ordinary
  SemVer tag through input, `GITHUB_REF`, `GITHUB_SHA`, checkout, and
  coordinated version files. Every image build needs that validation job,
  checks out its tag output; never restore branch-ref checkout with
  independently supplied publish tag.
- Every job running `actions/attest-build-provenance@*` needs
  `attestations: write` in its `permissions:` block. Adding new GPU
  variant without this permission silently disables GitHub-native
  attestation for that variant.
- Every container build job must generate CycloneDX SBOM with syft, attach
  it to same digest with `cosign attest`, upload JSON artifact.
  These steps are release gates: never add `continue-on-error` or other
  best-effort handling. Workflow summary must require every build job to
  finish with `success`; skipped GPU or server build is not accepted
  release result.
- Each Docker workflow's smoke job must run `cosign verify` against every
  freshly-pushed image it consumes before pulling and running it. Skipping
  this verification
  would re-open gap that ADR-0902 §G3 closed (compromised CI token
  pushes unsigned image; smoke test passes).
- Production GPU smoke consumes digest output from all three vendor
  build jobs, verifies each signature, then runs driver-independent
  `--version` entrypoint. Keep it in summary gate; GPU hardware is not
  required to catch broken runtime dependency closure.
- Certificate-identity regex in
  [`release.md`](../../docs/development/release.md) §"Consumer verification
  recipes" assumes workflow file path
  `.github/workflows/docker-publish-production.yml` and
  `.github/workflows/docker-publish-operator-node.yml` and
  `.github/workflows/supply-chain.yml`. Renaming or splitting these
  workflows requires updating both docs AND any cached consumer
  scripts (deprecated regex stays valid for old image digests in Rekor).
- `cosign-installer` SHA-pin: every install step uses same pinned v4 SHA.
  When Renovate or manual bump updates it, every build/smoke job in both
  Docker workflows and both `supply-chain.yml` jobs must move together.
  Mixed-version chain produces signature-format mismatches, surfacing
  only at consumer-verify time.
- Server-side `release-publish` / `pypi-publish` environment preflight,
  deployment-branch policy, and reviewer binding rules for `supply-chain.yml`
  and `docker-publish-*.yml` live in
  [publication-environment-binding](../../scripts/release/AGENTS.d/publication-environment-binding.md).
