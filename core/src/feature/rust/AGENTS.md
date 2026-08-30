<!-- markdownlint-disable MD013 MD029 -->
# AGENTS.md — core/src/feature/rust/

Rust feature extractor crates (ADR-0707 cbindgen pilot).

## Rebase-sensitive invariants

1. **`tad_rust.c` is a direct source of `library('vmaf')`** (in `core/src/meson.build`),
   NOT a member of `libvmaf_feature_sources`. This is intentional: LTO causes the linker
   to require `libvmafx_tad.a` at every link step that includes the TU. Keeping it
   out of the intermediate static lib avoids breaking static-lib-linked test executables
   that do not carry `rust_tad_dep`. Do not move it into `libvmaf_feature_sources`.

2. **`HAVE_RUST_TAD` gates the extractor in `feature_extractor.c`**. When adding a new
   Rust extractor, follow the same `#if HAVE_<NAME>` pattern so static-lib builds compile
   without the archive.

3. **`panic = "abort"` in `Cargo.toml` `[profile.release]`** is mandatory. Without it,
   the Rust staticlib references `rust_begin_unwind` (requires libstd), which breaks
   the C shared library link. All crates in this workspace must inherit this profile.

4. **Each crate is a workspace member**. Add new crates to `[workspace] members` in the
   root `Cargo.toml`; do not create standalone (non-workspace) crates under this tree.

5. **cbindgen-generated headers are not committed**. They live in `$OUT_DIR/include/`
   (produced by `cargo build`). C wrappers re-declare the ABI signatures directly rather
   than `#include`-ing the generated header, to avoid a source-tree dependency on a
   build artifact.

6. **`publish = false` on every fork-license crate in this tree.** `vmafx-tad` declares
   `BSD-3-Clause-Plus-Patent`, which the SPDX parser used by `cargo-deny` 0.19.8 does not
   yet recognise. The `publish = false` flag opts the crate out of crates.io publishing
   AND makes `[licenses.private] ignore = true` in `deny.toml` skip the license check
   for the workspace member. New Rust feature extractors using the fork license MUST
   follow the same pattern (or use a plain `BSD-3-Clause` declaration that the parser
   recognises). See [ADR-0917](../../../../docs/adr/0917-cargo-deny-supply-chain-policy.md).
6. **Codegen-only `build-dependencies` get a `[package.metadata.cargo-machete]`
   ignore**. `cargo-machete --with-metadata` does not introspect `build.rs` symbol
   usage, so build-only deps (`bindgen`, `cbindgen`, etc.) are mis-flagged as
   unused. Every new Rust crate whose only `[build-dependencies]` are codegen
   tools consumed by `build.rs` must add `[package.metadata.cargo-machete]`
   with `ignored = [<dep-names>]` and an inline comment citing the usage.
   See ADR-0904.
