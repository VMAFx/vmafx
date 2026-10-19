- **A build with `-Denable_rust_features=true` registers TAD and no longer
  exports the Rust standard library from `libvmaf.so`
  ([ADR-1713](docs/adr/1713-rc4-rust-extractor-framework.md)).** The TAD
  pilot was compiled but never registered (`--feature tad` failed with
  "problem loading feature extractor"), because the define that gated it never
  reached `feature_extractor.cpp`. The Rust archive's symbols are now kept out
  of the dynamic symbol table with `--exclude-libs` (GNU ld, lld). The Rust
  build also needs no network any more: TAD's unused build-time cbindgen
  dependency is gone and cargo runs `--offline --locked`.
