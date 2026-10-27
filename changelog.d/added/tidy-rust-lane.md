- **clang-tidy measures the sources built with `enable_rust_features` (ADR-3380).**
  A new tidy lane, `rust`, installs the pinned Rust toolchain in the dev
  container, configures the core with `-Denable_rust_features=true` and runs the
  ratchet on the Rust shims, `tad_rust.c` and their tests; it is the required
  check `Tidy Lane (rust)`, routed by its own paths, and part of the nightly
  sweep (#2880).
