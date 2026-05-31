# AGENTS.md — vmafx-sys

Parent: [../../../AGENTS.md](../../../AGENTS.md). Established by
[ADR-0707](../../../docs/adr/0707-vmafx-rust-pilot-feature.md) (Rust pilot

+ cbindgen) — this is the `-sys` FFI crate; a safe wrapper crate
(`bindings/rust/vmafx/`) is in scope for a follow-up.

## Rebase-sensitive invariants

- **bindgen allowlist**: `build.rs` uses `allowlist_function("vmaf_.*")`, `allowlist_type("Vmaf.*")`, and `allowlist_var("VMAF_.*")`. Do not widen this to `.*` — it would pull in OS internal types and break compilation. Widening requires an ADR.
- **safe layer unsafe boundary**: `unsafe` must remain confined to the FFI call sites inside `src/safe.rs`. Adding `unsafe` to safe-layer function signatures breaks the contract of the module.
- **`links = "vmaf"`**: The `links` field in `Cargo.toml` tells Cargo this crate provides the native `vmaf` library. Only one crate in a build graph may set `links = "vmaf"`. Do not add a second crate with the same links key.
- **No `pkg-config` dependency**: `build.rs` locates libvmaf via `LIBVMAF_PREFIX` only. This is intentional — `pkg-config` is not universally available (cross-compile targets, minimal CI containers). Do not add a `pkg-config` dep without an ADR.
- **Test data paths**: integration tests and examples resolve YUV/model paths relative to `VMAFX_REPO` (or auto-detected from `CARGO_MANIFEST_DIR`). Never hardcode absolute paths into test source.
- **Supply-chain policy (`deny.toml`)**: every new dep — direct or transitive — must pass `cargo deny check`. License allowlist is permissive-only (Apache-2.0, BSD-3-Clause, ISC, MIT, Unicode-3.0, Unlicense; MPL-2.0 narrowly allowed for `cbindgen`). `openssl-sys` and `native-tls` are banned in favour of rustls. Adding a banned crate, a copyleft license, or a non-crates.io source requires updating `deny.toml` AND citing the ADR / research digest that approves the exception. See [ADR-0917](../../../docs/adr/0917-cargo-deny-supply-chain-policy.md) and [`docs/development/cargo-deny.md`](../../../docs/development/cargo-deny.md).
- **`bindgen` is a `[package.metadata.cargo-machete] ignored` dep**: `cargo-machete --with-metadata` mis-flags build-only crates as unused. Do not remove the `ignored = ["bindgen"]` entry or `cargo-machete` audits will start surfacing a noisy false positive every run. See ADR-0904.
