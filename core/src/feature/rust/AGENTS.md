<!-- markdownlint-disable MD013 MD029 -->
# AGENTS.md — core/src/feature/rust/

Rust feature extractor crates (ADR-0707 cbindgen pilot).

## Rebase-sensitive invariants

1. **`tad_rust.c`** = direct source of `library('vmaf')` (in
   `core/src/meson.build`), not member of `libvmaf_feature_sources`.
   Intentional: LTO -> linker needs `libvmafx_tad.a` at every link step
   incl. the TU. Out of intermediate static lib -> avoids breaking
   static-lib-linked test executables without `rust_tad_dep`. Never move
   into `libvmaf_feature_sources`.

2. **`HAVE_RUST_TAD` gates extractor in `feature_extractor.c`**. New Rust
   extractor -> same `#if HAVE_<NAME>` pattern, so static-lib builds
   compile without archive.

3. **`panic = "abort"` in `Cargo.toml` `[profile.release]`** = mandatory.
   Without it: Rust staticlib references `rust_begin_unwind` (needs
   libstd) -> breaks C shared library link. Every crate in this workspace
   must inherit this profile.

4. **Each crate = workspace member**. New crates -> add to
   `[workspace] members` in root `Cargo.toml`. Never create standalone
   (non-workspace) crates under this tree.

5. **cbindgen-generated headers not committed**. Live in
   `$OUT_DIR/include/` (from `cargo build`). C wrappers re-declare ABI
   signatures directly, no `#include` of generated header — avoids
   source-tree dependency on build artifact.

6. **`publish = false` on every fork-license crate in this tree.**
   `vmafx-tad` declares `BSD-3-Clause-Plus-Patent`; SPDX parser in
   `cargo-deny` 0.19.8 does not yet recognise it. `publish = false` opts
   crate out of crates.io publishing AND makes
   `[licenses.private] ignore = true` in `deny.toml` skip license check
   for workspace member. New Rust feature extractors on fork license
   MUST follow same pattern (or use plain `BSD-3-Clause` declaration
   parser recognises). See
   [ADR-0917](../../../../docs/adr/0917-cargo-deny-supply-chain-policy.md).
6. **Codegen-only `build-dependencies` get `[package.metadata.cargo-machete]`
   ignore**. `cargo-machete --with-metadata` does not introspect
   `build.rs` symbol usage, so build-only deps (`bindgen`, `cbindgen`,
   etc.) get mis-flagged unused. Every new Rust crate whose only
   `[build-dependencies]` are codegen tools consumed by `build.rs` must
   add `[package.metadata.cargo-machete]` with `ignored = [<dep-names>]`
   and inline comment citing usage. See ADR-0904.
