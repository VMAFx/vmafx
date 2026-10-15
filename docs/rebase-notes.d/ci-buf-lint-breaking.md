## buf lint and breaking in CI (2026-10-15)

`ci/buf-lint-breaking`, [ADR-3329](adr/3329-ci-buf-lint-breaking.md). The `buf` job of
`.github/workflows/lint-and-format.yml` runs the `buf` program directly (`buf lint`,
`buf breaking`); keep it a direct step, because `praetorctl audit` counts a declared linter only
when a workflow step runs it, not through `scripts/codegen/proto_generate.py`. `BUF_VERSION` in
`build-config.env` stays the one buf pin.
