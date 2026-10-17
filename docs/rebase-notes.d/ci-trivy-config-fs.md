## Trivy config and filesystem scans in CI (2026-10-15)

`ci/trivy-lint`, [ADR-3330](adr/3330-ci-trivy-config-fs.md). The `trivy` job of
`.github/workflows/lint-and-format.yml` runs the `trivy` program directly (`trivy config`,
`trivy fs`); keep it a direct step, because `praetorctl audit` counts a declared linter only
when a workflow step runs it directly or uses an action with `trivy` in its repository name.
`TRIVY_VERSION` and `TRIVY_SHA256` in `build-config.env` pin the release binary. Dockerfile
exceptions are maintained with category and expiry in `.trivyignore.yaml`.
