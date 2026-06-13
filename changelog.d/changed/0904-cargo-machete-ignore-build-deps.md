- Pin `[package.metadata.cargo-machete] ignored = [...]` entries for the
  two Rust workspace crates so `cargo-machete --with-metadata` audits stay
  clean across the `bindgen` (vmafx-sys) and `cbindgen` (vmafx-tad)
  build-only dependencies. Both deps are consumed by `build.rs` for FFI
  binding / C header codegen; `--with-metadata` mis-classifies build-only
  deps as unused. See ADR-0904.
