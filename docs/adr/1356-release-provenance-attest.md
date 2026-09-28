<!-- markdownlint-disable MD013 MD060 -->
# ADR-1356: Release provenance from GitHub build-provenance attestations, not slsa-github-generator

- **Status**: Accepted
- **Date**: 2026-09-29
- **Deciders**: lusoris
- **Tags**: release, supply-chain, slsa, attestation, sigstore, github-actions, ci, security, fork-local

## Context

`.github/workflows/supply-chain.yml` produced release provenance for the native Linux files (`libvmaf.so*`, `vmaf`, `models.tar.gz`, `container-build-provenance.txt`) and for the `vmaf-mcp` wheel and sdist with `slsa-framework/slsa-github-generator/.github/workflows/generator_generic_slsa3.yml@v2.1.0`. The generator has been part of the release workflow since the first CI commit; [ADR-0166](0166-mcp-server-release-channel.md) extended it to `vmaf-mcp`, and [ADR-1305](1305-hash-locked-python-installs.md) kept its `@vX.Y.Z` reference as the only exception to commit-SHA pinning.

The VMAFx organisation and the repository both set `sha_pinning_required: true` (`gh api orgs/VMAFx/actions/permissions`). GitHub enforces that policy on every action a run loads, including actions a called reusable workflow references. The generator's reusable workflow calls its own sub-actions by tag, so a caller cannot satisfy the policy: in the v1.0.0-rc.2 publication (run 36488303062, attempt 1) both `detect-env` jobs failed with "The action slsa-framework/slsa-github-generator/.github/actions/detect-workflow-js@v2.1.0 is not allowed in VMAFx/vmafx because all actions must be pinned to a full-length commit SHA", and `Attach release assets` and `Publish vmaf-mcp PyPI`, which need the provenance, were skipped. Pinning the caller reference to a SHA would not help: the inner references stay tags, and the generator's builder verification needs a tag reference. v2.1.0 (2025-02-24) is the latest generator release, and the Renovate dependency dashboard (issue 941) lists the generator under "Abandoned Dependencies". The rc.2 release was recovered with a temporary policy relaxation, which is not a repeatable release step.

The container images already carry GitHub-native provenance from `actions/attest-build-provenance` ([ADR-0902](0902-signing-and-attestation-audit.md)). That action is a composite whose only step is `actions/attest` pinned by SHA, which runs as a Node 24 action with no further action references, so it runs under the organisation policy.

## Decision

We will produce release-blob and `vmaf-mcp` provenance with `actions/attest-build-provenance`, pinned by commit SHA like every other action (v4.2.2, `4d101475d8b20a2381f78447822ac1eab6504dd8`), and remove `slsa-github-generator` from the repository. This supersedes the generator provisions of ADR-0166 and the tag-pin exception in ADR-1305. Two plain jobs, `provenance` (native files) and `mcp-provenance` (wheel and sdist), replace the two generator calls. Each runs in the protected `release-publish` environment with exactly `contents: read`, `id-token: write` and `attestations: write`. Its subjects are the `hashes` output of the job that built the files, as the generator's `base64-subjects` were, and the job diffs those digests against the downloaded bytes. It then runs the documented consumer command, `gh attestation verify FILE --bundle BUNDLE --repo VMAFx/vmafx --signer-workflow VMAFx/vmafx/.github/workflows/supply-chain.yml --source-ref refs/tags/<tag>`, on every subject and uploads the Sigstore bundle as a workflow artifact. The environment-gated `attach-to-release` job remains the only release writer and attaches `vmafx-build-provenance.sigstore.json` and `vmaf-mcp-provenance.sigstore.json` in place of the two `.intoto.jsonl` files, so a consumer can verify offline. PyPI keeps its PEP 740 attestations unchanged.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep the generator and relax SHA pinning for the organisation or repository | No workflow change; keeps the generator's reusable-workflow isolation (the basis of its SLSA Build L3 claim) | Weakens an organisation security policy for every workflow and action to accommodate one dependency; a relaxation per release is a manual, error-prone step; the generator is unmaintained | Rejected: organisation security policy |
| Fork or vendor the generator with SHA-pinned internal references | Keeps the generator's predicate and isolation model | The fork becomes the trusted builder, so `slsa-verifier`'s builder allowlist no longer recognises it; the project would maintain an abandoned upstream's workflows, actions and Go builder | Rejected: maintenance of an abandoned upstream, and the builder identity is lost anyway |
| Pin only the caller reference to a SHA | One-line change | The generator's inner references stay tags, so the policy still rejects the run; the generator's builder verification needs a tag reference | Does not fix the failure |
| **`actions/attest-build-provenance` in plain jobs behind `release-publish` (chosen)** | SHA-pinned end to end; same mechanism as the container images (ADR-0902); a plain job can carry an environment, which ends the documented exception that let the reusable provenance jobs run outside `release-publish`; `gh attestation verify` works online and, with the attached bundle, offline; maintained by GitHub | GitHub documents attestations produced in the build workflow itself as SLSA v1.0 Build Level 2, not Level 3; consumers move from `slsa-verifier` to `gh attestation verify`; two more jobs wait for the release reviewer | Chosen |
| Use `actions/attest` directly | The attest-build-provenance README recommends it for new work; v4 of attest-build-provenance is a thin wrapper over it | Different action from the container workflows, which use attest-build-provenance; same attestation result | Kept one action across all release workflows; switching every workflow to `actions/attest` is a separate, mechanical follow-up |
| Isolate attestation in a repository-owned reusable workflow to claim SLSA Build L3 | Matches GitHub's documented Level 3 pattern | New reusable workflow and trust boundary to design and test without a release to exercise it; not needed to restore releases | Deferred; not needed to unblock v1.0.0-rc.3 |

## Consequences

- **Positive**:
  - Release provenance runs under `sha_pinning_required`; every `uses:` in `.github/workflows/` is now SHA-pinned with no exception, and `scripts/release/tests/test-publication-environment-binding.sh` rejects an unpinned action or a `slsa-github-generator` reference in the release workflows.
  - The provenance jobs join the `release-publish` environment, so every job holding OIDC or write scope in `supply-chain.yml` is reviewer-gated.
  - Each release proves, before attachment, that its bundles verify with the published consumer command for every subject.
  - Release blobs, `vmaf-mcp` distributions and container images share one provenance format and one verification tool.
- **Negative**:
  - The provenance is SLSA v1 build provenance at Build Level 2 by GitHub's own classification. The documentation no longer claims SLSA L3.
  - Existing `slsa-verifier` recipes stop working for new releases; v1.0.0-rc.1 and rc.2 keep their `.intoto.jsonl` files, and the release guide keeps the `slsa-verifier` command for them.
  - The two new `release-publish` jobs add deployment approvals: `mcp-provenance` becomes ready with `mcp-sign`, `provenance` shortly before `sign`.
- **Neutral / follow-ups**:
  - The attestation path runs only on a published release; pull-request CI covers it with actionlint and the release contract test, not by executing it. The first real run is the next candidate.
  - `.config/archetypes/` still declares `slsa_level: 3` for the praetor archetype; that is fleet configuration, not a release claim, and is left to the praetor owners.

## References

- Supersedes the `slsa-github-generator` provisions of [ADR-0166](0166-mcp-server-release-channel.md) and decision 8 (tag-pin exception) of [ADR-1305](1305-hash-locked-python-installs.md); extends [ADR-0902](0902-signing-and-attestation-audit.md) to release blobs; keeps [ADR-1151](1151-vmafx-first-release-1-0-0.md)'s environment model.
- Research digest: [docs/research/1356-release-provenance-attest.md](../research/1356-release-provenance-attest.md).
- GitHub: [artifact attestations](https://docs.github.com/en/actions/concepts/security/artifact-attestations) ("Artifact attestations by itself provides SLSA v1.0 Build Level 2"); [`actions/attest`](https://github.com/actions/attest) at `508db95dd578ae2727ebd6217d5ba78e4fbda05d`; [`gh attestation verify`](https://cli.github.com/manual/gh_attestation_verify).
- [SLSA v1.0 provenance](https://slsa.dev/spec/v1.0/provenance).
- Source: `req` "fix that, it was broken last time as well" (user, 2026-09-29).
