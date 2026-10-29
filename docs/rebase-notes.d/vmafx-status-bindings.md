## Generated Go and Rust status tables (2026-10-28)

`scripts/codegen/vmafx_api/emit_go.py` and `emit_rust.py` generate
`pkg/vmafx/status_gen.go` and `bindings/rust/vmafx/src/status_gen.rs` from
`core/api/vmafx.toml`, beside the existing Python `Status` enum.
For generated-file conflicts, take either side, resolve definition and emitter
changes, then run `python3 scripts/codegen/vmafx-api.py --write` and `--check`.
Keep both new renderers in `cli.py`'s `FIXED_OUTPUTS`, the Rust module and
re-export in `lib.rs`, and the status binding parity test in the tooling suite.
