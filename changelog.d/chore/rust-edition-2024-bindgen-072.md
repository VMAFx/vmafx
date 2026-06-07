### chore(rust): bump bindgen 0.69 → 0.72 and workspace edition 2021 → 2024

Resolves the deferral recorded in ADR-1000 / PR #592. bindgen 0.70+ emits
`unsafe extern "C"` blocks compatible with Rust edition 2024; bumping to 0.72.1
(latest stable) unblocks the edition upgrade.

**Changes:**

- `Cargo.toml` (workspace root): `edition = "2021"` → `"2024"`.
- `bindings/rust/vmafx/Cargo.toml`: hardcoded `edition = "2021"` replaced by
  `edition.workspace = true` to stay in sync with the workspace root.
- `bindings/rust/vmafx-sys/Cargo.toml`: `bindgen = "0.69"` → `"0.72"`.
- `core/src/feature/rust/tad/src/lib.rs`: edition-2024 migration —
  `extern "C"` → `unsafe extern "C"`, `#[no_mangle]` → `#[unsafe(no_mangle)]`,
  and raw-pointer dereferences / unsafe calls wrapped in explicit `unsafe {}` blocks.

All 21 workspace tests pass; `cargo clippy -- -D warnings` is clean.
