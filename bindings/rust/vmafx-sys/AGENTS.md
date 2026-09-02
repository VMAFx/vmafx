<!-- markdownlint-disable MD013 -->
# AGENTS.md — vmafx-sys

Parent: [../../../AGENTS.md](../../../AGENTS.md). Established by
[ADR-0707](../../../docs/adr/0707-vmafx-rust-pilot-feature.md) (Rust pilot
\+ cbindgen) — this is the `-sys` FFI crate; a safe wrapper crate
(`bindings/rust/vmafx/`) is in scope for a follow-up.

## Rebase-sensitive invariants

- **bindgen allowlist**: `build.rs` uses `allowlist_function("vmaf_.*")`, `allowlist_type("Vmaf.*")`, and `allowlist_var("VMAF_.*")`. Do not widen this to `.*` — it would pull in OS internal types and break compilation. Widening requires an ADR.
- **safe layer unsafe boundary**: `unsafe` must remain confined to the FFI call sites inside `src/safe.rs`. Adding `unsafe` to safe-layer function signatures breaks the contract of the module.
- **`links = "vmaf"`**: The `links` field in `Cargo.toml` tells Cargo this crate provides the native `vmaf` library. Only one crate in a build graph may set `links = "vmaf"`. Do not add a second crate with the same links key.
- **No `pkg-config` dependency**: `build.rs` locates libvmaf via `LIBVMAF_PREFIX` only. This is intentional — `pkg-config` is not universally available (cross-compile targets, minimal CI containers). Do not add a `pkg-config` dep without an ADR.
- **Test data paths**: integration tests and examples resolve YUV/model paths relative to `VMAFX_REPO` (or auto-detected from `CARGO_MANIFEST_DIR`). Never hardcode absolute paths into test source.
- **Supply-chain policy (`deny.toml`)**: every new dep — direct or transitive — must pass `cargo deny check`. License allowlist is permissive-only (Apache-2.0, BSD-3-Clause, ISC, MIT, Unicode-3.0, Unlicense; MPL-2.0 narrowly allowed for `cbindgen`). `openssl-sys` and `native-tls` are banned in favour of rustls. Adding a banned crate, a copyleft license, or a non-crates.io source requires updating `deny.toml` AND citing the ADR / research digest that approves the exception. See [ADR-0917](../../../docs/adr/0917-cargo-deny-supply-chain-policy.md) and [`docs/development/cargo-deny.md`](../../../docs/development/cargo-deny.md).
- **`bindgen` is a `[package.metadata.cargo-machete] ignored` dep**: `cargo-machete --with-metadata` mis-flags build-only crates as unused. Do not remove the `ignored = ["bindgen"]` entry or `cargo-machete` audits will start surfacing a noisy false positive every run. See ADR-0904.
- **bindgen minimum version is 0.70**: the workspace uses Rust edition 2024, which requires `unsafe extern "C"` blocks in generated bindings. bindgen < 0.70 emits bare `extern "C"` and will not compile under edition 2024. Do not downgrade below 0.70. See [ADR-1002](../../../docs/adr/1002-rust-edition-2024-bindgen-072.md).
- **`allow(clippy::all)` is scoped to generated bindings only** (ADR-1063): `src/lib.rs` wraps the `include!(bindings.rs)` in a private `mod bindings { #[allow(...)] ... }` and re-exports via `pub use bindings::*`. Do not lift the allow to the crate root — that would suppress clippy on the hand-written `safe.rs` module. New hand-written code in this crate must be clippy-clean.
- **No panicking `Default` impl** (ADR-1063): `VmafContext` does not implement `Default`. Callers must call `VmafContext::new()` and handle the `Result`. Do not add a `Default` impl that calls `expect` / `unwrap`.
- **`unsafe_op_in_unsafe_fn` is denied in `safe.rs`** (ADR-1063): every unsafe operation inside an `unsafe fn` body must be wrapped in an explicit `unsafe {}` block. Adding a bare unsafe operation inside an `unsafe fn` is a compile error.
- **`read_pictures` consumes pictures by value**: `VmafContext::read_pictures` takes both `VmafPicture` arguments **by value** (move), not `&mut`. Ownership of the plane buffers transfers to libvmaf, so the caller cannot follow the call with `unref_picture` and double-free — use-after-move is a compile error. Do **not** manually `unref` on the error path: the libvmaf public contract takes ownership for the duration of the call (a second unref is a use-after-free against a CUDA-enabled libvmaf), matching the higher-level `vmafx` crate's `Context::read_pictures` (PR #1056, round-3 R3-2). Do not revert `read_pictures` to a borrowing signature, and keep the two crates' picture-ownership models aligned. `unref_picture` stays `pub` only for pictures that were **never** handed to `read_pictures` (e.g. a partially-filled end-of-stream frame).
