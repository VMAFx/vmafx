- **Every Rust build uses one pinned toolchain, Rust 1.99.0.** `RUST_VERSION`
  in `build-config.env` sets it for `rust-toolchain.toml`, both Cargo
  workspaces' `rust-version`, the `Rust` workflow and the dev container, which
  now installs Rust and builds libvmaf with the Rust extractors.
  `-Denable_rust_features=true` without cargo now stops configure with an error
  instead of building the C extractors only (ADR-3318).
