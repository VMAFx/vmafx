- **Rust (vmafx-sys, vmafx, vmafx-tad)**: Remove panicking `Default` impl from
  `VmafContext` (library code must not hide abort paths behind trait impls);
  scope `#![allow(clippy::all)]` to the machine-generated bindgen output only
  (hand-written `safe.rs` is now covered by the full clippy suite);
  add `#![deny(unsafe_op_in_unsafe_fn)]` to `vmafx-sys/src/safe.rs` and
  `vmafx-tad/src/lib.rs`; add `#![warn(clippy::expect_used, clippy::unwrap_used)]`
  to the `vmafx` library crate.  Test modules opt-in with a local `#[allow(...)]`.
  (ADR-1063)
