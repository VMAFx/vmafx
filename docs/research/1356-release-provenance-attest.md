<!-- markdownlint-disable MD013 MD060 -->
# Research-1356: Release provenance under the organisation's SHA-pinning policy

- **Status**: Active
- **Workstream**: [ADR-1356](../adr/1356-release-provenance-attest.md)
- **Last updated**: 2026-09-29

## Question

Why did the v1.0.0-rc.2 provenance jobs fail, can `slsa-github-generator` run under the VMAFx policy at all, and does `actions/attest-build-provenance` give release blobs and `vmaf-mcp` distributions provenance that consumers can verify online and offline?

## Sources

- `gh api orgs/VMAFx/actions/permissions` and `gh api repos/VMAFx/vmafx/actions/permissions`, 2026-09-29.
- `supply-chain` run 36488303062 (v1.0.0-rc.2), attempt 1 job list and the log of job 109151051077 (`SLSA libvmaf / detect-env`).
- `actions/attest-build-provenance` v4.2.2 (`4d101475d8b20a2381f78447822ac1eab6504dd8`) `action.yml` and README; `actions/attest` at `508db95dd578ae2727ebd6217d5ba78e4fbda05d` `action.yml`, `src/main.ts` and `src/subject.ts`.
- GitHub documentation, [artifact attestations](https://docs.github.com/en/actions/concepts/security/artifact-attestations).
- `gh` 2.101.0 (`gh attestation verify`, `download`, `trusted-root`) and `slsa-verifier` v2.7.1 on the maintainer workstation, against the published v1.0.0-rc.2 assets and CPU image.
- Renovate dependency dashboard, issue 941.

## Findings

### The failure

Both the organisation and the repository return `"sha_pinning_required": true`. In attempt 1 of run 36488303062, `SLSA libvmaf / detect-env` and `SLSA vmaf-mcp / detect-env` failed; their `generator` jobs, `Publish vmaf-mcp PyPI` and `Attach release assets` were skipped. The job log reads:

```text
##[error]The action slsa-framework/slsa-github-generator/.github/actions/detect-workflow-js@v2.1.0 is not allowed in VMAFx/vmafx because all actions must be pinned to a full-length commit SHA.
```

The caller referenced the reusable workflow by tag, as the generator requires, but the rejected reference is the generator's own `detect-workflow-js@v2.1.0` inside its workflow. The policy therefore applies to actions nested in a called reusable workflow, and no caller-side change can satisfy it. Attempt 2, after a temporary policy relaxation, succeeded and attached the `.intoto.jsonl` files. The dashboard lists `slsa-framework/slsa-github-generator` under "Abandoned Dependencies", last updated 2025-02-24 (v2.1.0).

### The replacement action

`attest-build-provenance` v4.2.2 is a composite action with one step, `actions/attest@508db95dd578ae2727ebd6217d5ba78e4fbda05d`. `actions/attest` declares `runs: using: node24` and references no other action, so the whole chain is SHA-pinned. Its README says v4 is a wrapper and recommends `actions/attest` for new work; the container workflows already use `attest-build-provenance` at the same SHA.

- **Subjects**: `subject-checksums` takes a file in `sha256sum` format; the parser takes the digest before the first space and strips one `*` or space flag before the name. `sha256sum -- *` output, which `build-artifacts` and `mcp-build` already publish base64-encoded as their `hashes` output, has that shape. Several subjects produce one attestation.
- **Bundle**: `bundle-path` is `attestation.json` in a fresh `mkdtemp` directory under `RUNNER_TEMP`, holding the JSON-serialised Sigstore bundle on one line.
- **Permissions**: `id-token: write` and `attestations: write`. `artifact-metadata: write` is needed only for storage records, which require `push-to-registry`, so blob attestations do not need it.
- **SLSA level**: GitHub's documentation states "Artifact attestations by itself provides SLSA v1.0 Build Level 2" and that reusable workflows can provide the isolation for Build Level 3. The predicate type is `https://slsa.dev/provenance/v1`.

### Verification recipes, exercised

No release blob has a GitHub attestation yet, so the recipes were run against the v1.0.0-rc.2 CPU image, whose attestation comes from the same action. The image index bytes fetched anonymously from `ghcr.io/v2/vmafx/vmafx/manifests/v1.0.0-rc.2` hash to the image digest `sha256:9f15cd78…5ddb`, so the saved file stands in for a release blob in file mode.

| Command | Result |
|---|---|
| `gh attestation verify manifest.json --repo VMAFx/vmafx --signer-workflow VMAFx/vmafx/.github/workflows/docker-publish-production.yml --source-ref refs/tags/v1.0.0-rc.2` | exit 0; predicate `https://slsa.dev/provenance/v1`, SAN `…/docker-publish-production.yml@refs/tags/v1.0.0-rc.2` |
| the same with `--source-ref refs/tags/v9.9.9` | exit 1, `expected SourceRepositoryRef to be refs/tags/v9.9.9, got refs/tags/v1.0.0-rc.2` |
| `gh attestation download` → first line saved as `single.sigstore.json` (`application/vnd.dev.sigstore.bundle.v0.3+json`); `gh attestation trusted-root > trusted_root.jsonl`; `gh attestation verify manifest.json --bundle single.sigstore.json --custom-trusted-root trusted_root.jsonl --repo VMAFx/vmafx --signer-workflow … --source-ref refs/tags/v1.0.0-rc.2` | exit 0 |
| the same after appending one byte to `manifest.json` | exit 1 |
| `gh attestation verify vmaf --repo VMAFx/vmafx` on the rc.2 `vmaf` asset | exit 1, HTTP 404: the generator published no GitHub attestation |
| `slsa-verifier verify-artifact vmaf --provenance-path vmafx-build-provenance.intoto.jsonl --source-uri github.com/VMAFx/vmafx --source-tag v1.0.0-rc.2` | `PASSED`, builder `generator_generic_slsa3.yml@refs/tags/v2.1.0`, commit `c894eb9d0` |

The single-bundle JSON file that `gh attestation download` produces has the same format as the file `actions/attest` writes to `bundle-path`, so the release's `.sigstore.json` assets verify with the `--bundle` form, and offline once the trusted root is cached.

### What stays unexercised

The new jobs run only on a published release. Pull-request CI checks the workflow with actionlint and `scripts/release/tests/test-publication-environment-binding.sh`; the first real execution is the next candidate. The jobs verify their own bundle against every subject with the consumer command before the attachment job can run, so a bundle that fails verification stops that release before anything is published.

## Conclusion

`slsa-github-generator` cannot run under `sha_pinning_required`, whatever the caller does. `actions/attest-build-provenance` at a SHA pin runs under it, produces SLSA v1 build provenance at GitHub's stated Build Level 2, and gives consumers `gh attestation verify` online and, with the attached bundle and a cached trusted root, offline.
