<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3331: Run Spectral OpenAPI lint in CI

- **Status**: Accepted
- **Date**: 2026-10-15
- **Deciders**: lusoris
- **Tags**: ci, api, governance, standards

## Context

The repository maintains an OpenAPI 3.0 specification for the VMAFX scoring server under `api/openapi/vmafx-server-v1.yaml` ([ADR-1852](1852-vmafx-api-redesign.md), [ADR-2350](2350-cloud-native-platform.md)). The governance engine at `3bbde07` requires declared linters to run in CI workflows (`praetorctl audit`: "Declared linters: spectral ... not run by any workflow"), which blocks the engine pin. Although the specification is checked locally during development, no continuous integration workflow executed `@stoplight/spectral-cli`, allowing invalid paths, missing operation IDs, or malformed schema properties to enter unflagged.

## Decision

We will run Spectral in CI as the job `Spectral OpenAPI Lint` of `lint-and-format.yml`. The job downloads Spectral's standalone `spectral-linux-x64` release binary at `SPECTRAL_VERSION`, checks it against `SPECTRAL_SHA256` (both in `build-config.env`), and runs `spectral lint --fail-severity=warn api/openapi/vmafx-server-v1.yaml` using root `.spectral.yaml` (`extends: ["spectral:oas"]`). It follows the Lint workflow's tier gate and is not a required check.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Job in `lint-and-format.yml` (chosen) | Reuses the tier gate and draft skip; no new workflow file or routing registration | The Lint workflow grows by one light job | — |
| Separate `spectral.yml` workflow | Isolated workflow execution | Requires dedicated tier wiring and routing contract entries for a fast linter | Unnecessary workflow sprawl |
| `npx -y @stoplight/spectral-cli@<version>` | No binary download | Installs an unpinned npm dependency tree on every run; needs `setup-node` | The package version is pinned but its transitive dependencies are not |
| Third-party GitHub action (`stoplight/spectral-action`) | Pre-packaged action | Introduces an additional external action pin and slower container execution | A checksum-verified release binary is one pin, as for buf and trivy |
| Run Spectral via pre-commit only | Immediate local developer feedback | Engine audit requires direct workflow step execution; pre-commit node hook setup is heavier | Engine declared-linter gate does not count pre-commit wrappers |

## Consequences

- **Positive**: OpenAPI contract infractions fail in CI on pull requests; satisfies the governance engine declared-linter gate for spectral.
- **Negative**: Adds a light workflow job (~30-60 seconds) to pull request runs.
- **Neutral / follow-ups**: Paves the way for the engine pin update to `3bbde07` alongside the buf and trivy jobs.

## Verification

A planted defect (an operation under `/v1/planted_defect` without `operationId` or `description`) fails `spectral lint --fail-severity=warn` with non-zero exit code 1 (`operation-description`, `operation-operationId`, `operation-tags`). The valid spec exits 0 with 0 warnings/errors.

In CI, the `Spectral OpenAPI Lint` job of run 38802603401 (job 116666041222) failed on the planted commit `3c2f8dbe4` with the same three warnings at `paths./v1/planted-defect.get` and exit code 1; the defect was then reverted.

## References

- Operator decision `vmafx-engine-pin-declared-linters` (2026-10-15, paraphrased): one small pull request per declared linter, each seen failing on a planted defect, before the engine pin.
- [API generation](../development/api-generation.md), [ADR-1852](1852-vmafx-api-redesign.md), [ADR-2350](2350-cloud-native-platform.md), ADR-3329.
