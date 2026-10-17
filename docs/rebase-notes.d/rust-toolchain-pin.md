## One pinned Rust toolchain (2026-10-15)

- `build-config.env` `RUST_VERSION`, `rust-toolchain.toml`, the
  `rust-version` of `Cargo.toml` and `core/src/rust/Cargo.toml`,
  `.github/workflows/rust-ci.yml` and the rustup install of `dev/Containerfile`
  name one version (ADR-3318); `check-workflow-versions.py` compares them.
  Fork-only, nothing upstream to merge.
- `core/src/meson.build`: a missing cargo with `enable_rust_features=true` is
  an `error()`; `core/src/rust/build_staticlib.py` runs cargo in the manifest's
  directory. **On sync**: keep both; a new crate takes
  `rust-version.workspace = true`, and a new place that installs Rust reads
  `RUST_VERSION` and gets a selector in the Renovate `Rust toolchain` manager.
