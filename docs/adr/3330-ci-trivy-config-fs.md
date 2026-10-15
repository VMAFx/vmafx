<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3330: Run Trivy config and filesystem scans in CI

- **Status**: Accepted
- **Date**: 2026-10-15
- **Deciders**: lusoris
- **Tags**: ci, security, docker, governance, standards

## Context

The repository declares Trivy for the facet `security:high` and native container profiles ([ADR-0878](0878-trivy-container-scan-baseline.md)), checking Dockerfile IaC misconfigurations and repository vulnerabilities/secrets. While production container images run unprivileged and undergo release-time checks, no continuous workflow previously executed `trivy` on every pull request. The governance engine at `3bbde07` requires declared linters to run in CI workflows (`praetorctl audit`: "Declared linters: trivy ... not run by any workflow"), which blocks the engine pin.

## Decision

We will run Trivy in CI as the job `Trivy Config + FS` (`trivy:`) of `lint-and-format.yml`. The job installs the pinned `TRIVY_VERSION` ("0.76.0") release binary from GitHub releases and verifies its checksum against `TRIVY_SHA256`, both single-sourced in `build-config.env`. It executes:

1. `trivy config --exit-code 1 --ignorefile .trivyignore.yaml --severity HIGH,CRITICAL --skip-dirs .corpus,.workingdir,build .`
2. `trivy fs --scanners vuln,secret --exit-code 1 --ignorefile .trivyignore.yaml --severity HIGH,CRITICAL --skip-dirs .corpus,.workingdir,build .`

Non-runtime Dockerfile exceptions are maintained in `.trivyignore.yaml` with mandatory category (Category D, 120-day limit) and expiration dates (`expired_at`). The job follows the Lint workflow's tier gate (`needs.tier.outputs.light == 'true'`) and is not a required check.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Job in `lint-and-format.yml` (chosen) | Reuses the tier gate and draft skip; no new workflow to register in `.github/ci-tier.json` and the routing contract | The Lint workflow grows by one job | — |
| Separate `trivy.yml` workflow | Isolated triggers | Needs its own tier wiring, routing-contract and job-name registry entries for one small job | More moving parts for the same check |
| `aquasecurity/trivy-action` | Official GitHub Action | Pinned action wraps container download or runner setup; adds an extra action dependency and separate pin mechanism | Release binary download with SHA-256 verification in `build-config.env` aligns with Prometheus and dashboard-linter pins |
| Pre-commit hook only | Runs before commit | Slower local commit cadence, requires local Trivy installation on every workstation | CI provides a uniform gate; pre-commit remains focused on fast formatters and linters |

## Consequences

- **Positive**: Dockerfile misconfigurations (such as missing `USER` directives) and filesystem vulnerabilities/secrets fail the pull request that brings them; the engine's declared-linter gate counts Trivy.
- **Negative**: Adds approximately 30–60 seconds to pull request CI execution (binary download and scan).
- **Neutral / follow-ups**: Spectral, the remaining declared linter, gets its own pull request; then the engine pin moves to `3bbde07`.

## Verification

A planted defect (temporary `Dockerfile` containing `FROM ubuntu:latest` without a `USER` directive) failed `trivy config` with exit code 1 (`DS-0002`), and was verified clean (exit code 0) upon removal.

In CI, the planted-defect commit `eeddb1759` turned the `Trivy Config + FS` job red ([run 38802257680](https://github.com/VMAFx/vmafx/actions/runs/38802257680/job/116656535063)): `trivy config` reported `DS-0002` (HIGH) on `planted-trivy/Dockerfile` and `trivy fs` reported CVE-2018-18074 (HIGH) on `requests==2.19.0` in `planted-trivy/requirements.txt`, each with exit code 1. The revert that follows it removes both.

## References

- Operator decision `vmafx-engine-pin-declared-linters` (2026-10-15, paraphrased): one small pull request per declared linter, each seen failing on a planted defect, before the engine pin.
- [ADR-0878: Trivy container scan baseline](0878-trivy-container-scan-baseline.md).
- [Docker usage](../usage/docker.md#image-security-and-scanning), [CI overview](../development/ci.md).
