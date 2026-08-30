## `vmafx` safe Rust binding crate — Phase 1 scaffold

- **New crate `bindings/rust/vmafx`** ([ADR-0929](../docs/adr/0929-rust-safe-binding-scaffold.md)).
  Idiomatic Rust API sitting on top of `vmafx-sys`. Phase 1 ships the
  essential scoring loop: `Context` / `ContextBuilder` / `LogLevel`,
  `Model`, `Picture` / `PixelFormat`, `Score` / `PoolingMethod`, plus an
  `Error` enum with curated POSIX errno mapping (`ENOMEM`, `EINVAL`,
  `ENOSYS`/`ENOTSUP`, `EACCES`, `ENOENT`) and a `Libvmaf { code }`
  catch-all. All types are `Send` but not `Sync` — libvmaf does not
  document concurrent access on a single object. RAII `Drop` impls free
  every resource; `Context::read_pictures` consumes both `Picture`
  values so the buffers libvmaf takes ownership of are never
  double-freed by the Rust side.
- The legacy `vmafx_sys::safe::*` API is preserved unchanged. A
  follow-on PR will deprecate it once `vmafx` reaches feature parity.
- Added as a new workspace member in the root `Cargo.toml`. `cargo build
  --workspace` and `cargo test --workspace` cover both crates.
