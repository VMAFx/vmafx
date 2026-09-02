## Rust crate audit — TAD extractor and vmafx-sys bindings (Research-0760)

Security and correctness audit of the two Rust workspace crates:

- All `unsafe` blocks in `vmafx-tad` and `vmafx-sys/safe.rs` are covered
  by inline `// SAFETY:` or `# Safety` justification comments.
- cbindgen header drift check: no drift found; the manually-declared C signatures
  in `tad_rust.c` match the Rust source exactly, and the link step self-checks.
- `RUSTSEC-2022-0027` (`lazycell` interior-mutability unsoundness) noted as a
  build-time-only transitive dependency of `bindgen 0.69.5`; upgrade to `bindgen
  0.72.x` recommended but not urgent.
- Cargo.lock is fully deterministic with no out-of-tree or patched dependencies.
- ADR-0707 dispatch contract (`HAVE_RUST_TAD` + no-op `-ENOSYS` stubs)
  confirmed fully implemented.
- `enable_rust_features` default confirmed `false` in `meson_options.txt`;
  corrected an inaccurate "default true" claim in ADR-0707 body.

References: Research-0760, ADR-0707.
