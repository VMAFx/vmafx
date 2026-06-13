### VMAFX Phase 4 language-modernization foundation (ADR-0702)

- Added Go workspace root (`go.mod`, module `github.com/VMAFx/vmafx`, Go 1.23)
  with `pkg/version` package and `go-ci.yml` CI gate.
- Added Rust workspace root (`Cargo.toml`) with placeholder member directories
  (`bindings/rust/`, `core/src/feature/rust/`) and `rust-ci.yml` CI gate.
- Added `Makefile` targets `go-build`, `go-test`, `rust-build`, `rust-test`.
- Added multi-language policy section to `docs/principles.md` (§8).
- Added `docs/development/languages.md` describing every language used in the
  project, minimum versions, and per-language CI gates.
- Updated `.gitignore` for Go binaries and Rust `target/` directory.
