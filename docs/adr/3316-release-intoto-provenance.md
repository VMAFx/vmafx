<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3316: Release assets carry their build provenance

- **Status**: Accepted
- **Date**: 2026-10-14
- **Deciders**: Lusoris (operator order vmafx-red-main-20261013-scorecard-plan, paraphrased)
- **Tags**: ci, scorecard, security, release, supply-chain

## Context

OpenSSF Scorecard evaluation on `master` reported an aggregate repository score of 8.452381, which is below the project's 8.5 quality gate threshold enforced by `scripts/ci/scorecard_gate.py`.

Under Scorecard v5.6.0 rules (`checks/evaluation/signed_releases.go`), the Signed-Releases check inspects the last 5 releases. Each release scores:

- 8 if it carries cryptographic signature assets (e.g. `.sig`, `.asc`, `.sigstore`, `.sigstore.json`).
- 10 if it carries an in-toto build-provenance attestation asset whose filename ends in `.intoto.jsonl`.
The final check score is the floor of the releases' average.

At present, all five releases carry keyless Sigstore signatures (`*.sigstore.json`), but none carry an `.intoto.jsonl` asset, capping the check score at 8.

The native GitHub action `actions/attest-build-provenance` v4.2.2 (wrapping `actions/attest` v4.2.1) produces an output `bundle-path` containing `JSON.stringify(bundle) + EOL`. This file is valid single-line JSONL. Normalizing the bundle with `jq --compact-output . "$BUNDLE" > *.intoto.jsonl` ensures a single JSON line format. Consumers can verify release assets against the bundle offline via `gh attestation verify <asset> --bundle <bundle>.intoto.jsonl --repo VMAFx/vmafx`.

## Decision

Every workflow that creates a GitHub release or uploads release assets must normalize and publish its build-provenance bundle as an `*.intoto.jsonl` release asset alongside existing `.sigstore.json` signature files:

1. **macOS tester bundle (`.github/workflows/macos-tester-bundle.yml`)**: Assigns `id: provenance` to the `actions/attest-build-provenance` step. A subsequent step normalizes the bundle to `bundle/vmafx-tester-macos-provenance.intoto.jsonl` using `jq -c .`, asserts exactly one line, and verifies all `bundle/*.tar.xz` archives via `gh attestation verify`. The existing `bundle/*` glob in the release creation step attaches the file.
2. **Windows tester bundle (`.github/workflows/windows-tester-bundle.yml`)**: Assigns `id: provenance` to the `actions/attest-build-provenance` step. A subsequent step normalizes the bundle to `assets/vmafx-tester-windows-provenance.intoto.jsonl` using `jq -c .`, asserts exactly one line, and verifies all `assets/*.zip` archives via `gh attestation verify`. The existing `assets/*` glob in the release creation step attaches the file.
3. **Supply-chain release workflow (`.github/workflows/supply-chain.yml`)**: In both `provenance` (native binaries) and `mcp-provenance` (PyPI package) jobs, normalizes the staged bundle to `provenance/vmafx-build-provenance.intoto.jsonl` and `provenance/vmaf-mcp-provenance.intoto.jsonl`, asserts single-line format, and passes both files through artifact upload/download to `attach-to-release`. The release job requires both files and lists them in `softprops/action-gh-release` `files:`.
4. **Static CI checker (`scripts/ci/check_release_provenance_assets.py`)**: Inspects `.github/workflows/` using `yaml.safe_load`. Any workflow invoking `softprops/action-gh-release` or running `gh release create` / `gh release upload` (ignoring comments) is classified as a releaser. The checker requires that each releaser writes an `.intoto.jsonl` path in a `run:` step and includes it in its uploaded files/globs.
5. **Testing and regression suite (`scripts/ci/tests/test_check_release_provenance_assets.py`)**: Validates positive repository state, missing provenance negatives, unmatched glob negatives, stale exemption negatives, unparsable YAML exit code 2, and non-releaser boundaries (`# gh release create`, `gh release view`).
6. **Hook and pipeline wiring**: Integrated into `.pre-commit-config.yaml` (`check-release-provenance-assets`, `test-check-release-provenance-assets`), `Makefile` (`lint-actions`), and tracked under the `tooling` test suite executed by the "Tooling Tests" CI job.

### Out of scope workflows

- `docker-publish-*`: Out of scope because these workflows only read release tags to build and attest container images published to GitHub Packages (GHCR); they do not create GitHub releases or upload release assets.
- `release-please.yml`: Out of scope because it uses `googleapis/release-please-action` to draft release PRs and tags releases without releasing binary build assets.
- `published-rc-licence-companions.yml`: Out of scope because it attaches retrospective licence texts and third-party notices to historic candidate releases (v1.0.0-rc.1 and rc.2); it builds and releases no binaries, and is formally exempted in `EXEMPT_WORKFLOWS`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Do nothing / wait for SAST | Zero code changes | Scorecard Signed-Releases stays at 8; total score 8.452381 remains below 8.5 gate | Fails required security gate on `master` |
| Upload attestation store bundles by hand once | No workflow edits | Manual, unrepeatable, brittle across future releases, fails automation invariant | Unautomated manual state mutations are forbidden |
| Generate a separate SLSA generator workflow (`slsa-framework/slsa-github-generator`) | Upstream SLSA branding | Reintroduces unpinned action dependencies previously eliminated by ADR-1356 | Fails strict SHA-pinning and hermeticity invariants |
| Emit normalized `*.intoto.jsonl` from native `actions/attest-build-provenance` outputs and gate in CI (chosen) | Meets Scorecard 10/10 for Signed-Releases; zero new third-party actions; offline consumer verification | Uploads one additional provenance asset per release bundle | Fully satisfies supply chain, Scorecard, and gate invariants cleanly |

## Consequences

- **Positive**: Future releases achieve score 10 for Signed-Releases; repository overall Scorecard score advances past the 8.5 quality gate; consumers can verify release assets offline via `gh attestation verify --bundle <file>.intoto.jsonl`.
- **Negative**: Workflows write and upload one additional `.intoto.jsonl` asset per release bundle.
- **Neutral / follow-ups**: Existing 5 releases remain at score 8 unless backfilled by separate operator action (`vmafx-signed-releases-provenance`); documentation in `docs/development/release.md` and `docs/usage/tester-image.md` updated with consumer verification recipes.

## References

- Operator order `vmafx-red-main-20261013-scorecard-plan` (paraphrased).
- OpenSSF Scorecard v5.6.0: `checks/evaluation/signed_releases.go`.
- [ADR-1356](1356-release-provenance-attest.md), [ADR-1503](1503-tester-artifact-licensing.md), [ADR-3315](3315-scorecard-pinned-local-installs.md).
