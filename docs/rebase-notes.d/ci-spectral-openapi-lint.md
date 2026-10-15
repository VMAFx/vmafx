## Spectral OpenAPI lint in CI (2026-10-15)

`ci/spectral-lint`, [ADR-3331](adr/3331-ci-spectral-openapi-lint.md). The `spectral` job of
`.github/workflows/lint-and-format.yml` runs the checksum-verified Spectral release binary directly
(`spectral lint`); keep it a direct step, because `praetorctl audit`
counts a declared linter only when a workflow step runs it. `SPECTRAL_VERSION` and `SPECTRAL_SHA256` in
`build-config.env` stay the one spectral pin; move both together.
