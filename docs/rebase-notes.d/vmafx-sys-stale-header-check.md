## vmafx-sys refuses a stale libvmaf header (#2786) (2026-10-15)

- `bindings/rust/vmafx-sys/build.rs` checks the generated bindings against
  `REQUIRED_SYMBOLS` in `bindings/rust/vmafx-sys/build_support/header_check.rs`
  and fails with the header path and the missing names. Fork-only code; no
  upstream file is involved. **On sync**: a new libvmaf name used by
  `vmafx-sys/src/safe.rs` or `bindings/rust/vmafx/src` goes on the list;
  `cargo test -p vmafx-sys --test header_check` fails otherwise.
