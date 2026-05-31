### vmafx-sys Rust FFI crate

Add `bindings/rust/vmafx-sys` — bindgen-generated raw FFI bindings to libvmaf plus
a thin safe Rust wrapper layer (`vmafx_sys::safe`). The safe layer exposes
`VmafContext`, `VmafModel`, and YUV picture helpers with `Result`-based error
handling. A root-level Rust workspace (`Cargo.toml`) groups all future Rust crates.

CI gate (`rust-ci.yml`) runs `cargo fmt --check`, `cargo clippy -D warnings`,
`cargo test`, and the Netflix golden smoke test on every PR touching `bindings/rust/`.

See: ADR-0706, `docs/development/rust.md`.
