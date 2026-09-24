<!-- markdownlint-disable MD013 -->

# AGENTS.md — vmafx-sys

Parent: [../../../AGENTS.md](../../../AGENTS.md). Established by
[ADR-0707](../../../docs/adr/0707-vmafx-rust-pilot-feature.md) (Rust pilot
\+ cbindgen). This = `-sys` FFI crate; safe wrapper crate
(`bindings/rust/vmafx/`) in scope for follow-up.

## Rebase-sensitive invariants

- **bindgen allowlist**: `build.rs` uses `allowlist_function("vmaf_.*")`,
  `allowlist_type("Vmaf.*")`, `allowlist_var("VMAF_.*")`. Do not widen to
  `.*` -> pulls OS internal types, breaks compilation. Widening requires ADR.
- **safe layer unsafe boundary**: `unsafe` must remain confined to FFI call
  sites inside `src/safe.rs`. Adding `unsafe` to safe-layer function signatures
  breaks module contract.
- **`links = "vmaf"`**: `links` field in `Cargo.toml` tells Cargo crate provides
  native `vmaf` library. Only one crate in build graph may set
  `links = "vmaf"`. Do not add second crate with same links key.
- **No `pkg-config` dependency**: `build.rs` locates libvmaf via
  `LIBVMAF_PREFIX` only. `pkg-config` not universally available (cross-compile
  targets, minimal CI containers). Do not add `pkg-config` dep without ADR.
- **Test data paths**: integration tests and examples resolve YUV/model paths
  relative to `VMAFX_REPO` (or auto-detected from `CARGO_MANIFEST_DIR`). Never
  hardcode absolute paths into test source.
- **Supply-chain policy (`deny.toml`)**: every new dep (direct or transitive)
  must pass `cargo deny check`. License allowlist permissive-only: Apache-2.0,
  BSD-3-Clause, ISC, MIT, Unicode-3.0, Unlicense; MPL-2.0 narrowly allowed for
  `cbindgen`. `openssl-sys` and `native-tls` banned in favour of rustls.
  Adding banned crate, copyleft license, or non-crates.io source requires
  updating `deny.toml` AND citing ADR / research digest approving exception.
  See [ADR-0917](../../../docs/adr/0917-cargo-deny-supply-chain-policy.md) and
  [`docs/development/cargo-deny.md`](../../../docs/development/cargo-deny.md).
- **`bindgen` is a `[package.metadata.cargo-machete] ignored` dep**:
  `cargo-machete --with-metadata` mis-flags build-only crates as unused. Do not
  remove `ignored = ["bindgen"]` entry or `cargo-machete` audits surface noisy
  false positive every run. See ADR-0904.
- **bindgen minimum version is 0.70**: workspace uses Rust edition 2024;
  requires `unsafe extern "C"` blocks in generated bindings. bindgen < 0.70
  emits bare `extern "C"`, will not compile under edition 2024. Do not
  downgrade below 0.70. See
  [ADR-1002](../../../docs/adr/1002-rust-edition-2024-bindgen-072.md).
- **`allow(clippy::all)` is scoped to generated bindings only** (ADR-1063):
  `src/lib.rs` wraps `include!(bindings.rs)` in private
  `mod bindings { #[allow(...)] ... }`, re-exports via `pub use bindings::*`.
  Do not lift allow to crate root -> suppresses clippy on hand-written
  `safe.rs` module. New hand-written code in this crate must be clippy-clean.
- **No panicking `Default` impl** (ADR-1063): `VmafContext` does not implement
  `Default`. Callers must call `VmafContext::new()`, handle `Result`. Do not
  add `Default` impl calling `expect` / `unwrap`.
- **`unsafe_op_in_unsafe_fn` is denied in `safe.rs`** (ADR-1063): every unsafe
  operation inside `unsafe fn` body must be wrapped in explicit `unsafe {}`
  block. Adding bare unsafe operation inside `unsafe fn` = compile error.
- **`read_pictures` consumes pictures by value**: `VmafContext::read_pictures`
  takes both `VmafPicture` arguments **by value** (move), not `&mut`.
  Ownership of plane buffers transfers to libvmaf; caller cannot follow call
  with `unref_picture` and double-free -> use-after-move = compile error.
  Do **not** manually `unref` on error path: libvmaf public contract takes
  ownership for duration of call (second unref = use-after-free against
  CUDA-enabled libvmaf), matching higher-level `vmafx` crate
  `Context::read_pictures` (PR #1056, round-3 R3-2). Do not revert
  `read_pictures` to borrowing signature; keep two crates' picture-ownership
  models aligned. `unref_picture` stays `pub` only for pictures **never**
  handed to `read_pictures` (e.g. partially-filled end-of-stream frame).
- **CI impact coverage for public C headers**: `vmafx-sys` generates FFI bindings
  from `core/include/libvmaf/libvmaf.h` via `bindgen`. Keep
  `"core/include/libvmaf/**"` in `.github/ci-impact.json`'s Rust selector so public
  header changes run `vmafx-sys CI`. `.github/workflows/rust-ci.yml` must start
  without workflow-level path filters and emit its exact required gate names even
  when Rust work is not selected (BUG-098).
