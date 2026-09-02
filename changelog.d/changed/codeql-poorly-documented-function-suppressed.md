- **CodeQL config**: globally suppress the `cpp/poorly-documented-function`
  rule from the `security-and-quality` query pack
  ([`.github/codeql-config.yml`](.github/codeql-config.yml)). The rule
  flags every C/C++ function lacking a `/** */` Doxygen header block,
  which conflicts with the fork's coding standard ("default to writing
  no comments; only add one when the *WHY* is non-obvious"). 15
  currently-open alerts in `core/src/` close on the next scan. See
  [ADR-0348](docs/adr/0348-codeql-poorly-documented-function-suppressed.md).
  Remaining `security-extended` + `security-and-quality` rules stay
  enabled.
