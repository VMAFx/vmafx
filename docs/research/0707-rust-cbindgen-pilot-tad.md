<!-- markdownlint-disable MD013 -->
# Research: Rust/cbindgen Integration for libvmaf Feature Extractors (ADR-0707)

**Date**: 2026-05-28
**Branch**: feat/tad-rust-pilot
**ADR**: [ADR-0707](../adr/0707-vmafx-rust-pilot-feature.md)

## Objective

Establish whether a Rust `staticlib` can be linked into `libvmaf.so` via a Meson
`custom_target(cargo build)` + `declare_dependency` without disrupting existing tests,
build configurations, or the Netflix golden-data gate.

## Key findings

### 1. cbindgen header generation works correctly

`cbindgen 0.27` (current stable) generates a valid C header from Rust `#[no_mangle] pub unsafe extern "C"` functions. The header is written to `$OUT_DIR/include/vmafx_tad.h` during `cargo build`. The signatures are stable and simple enough that we re-declare them directly in the C wrapper (`tad_rust.c`) rather than depending on the generated header at compile time — this avoids a generated-file dependency in the Meson source tree.

### 2. LTO interaction requires architectural care

With `-flto=auto` (the repo default), the linker resolves all undefined symbols across all TUs in a link step, even those in dead code paths. Placing `tad_rust.c` in the `libvmaf_feature` intermediate static lib caused every test executable (including those that never call TAD) to require `libvmafx_tad.a` on their link line.

**Solution**: compile `tad_rust.c` as a direct source of the `library('vmaf')` target
(not into `libvmaf_feature.a`), and gate the `vmaf_fex_tad` extern + list entry in
`feature_extractor.c` behind `#if HAVE_RUST_TAD`. This way:

- Static-lib-linked test binaries see no TAD symbols at all.
- The shared `libvmaf.so` compiles `tad_rust.c` with `-DHAVE_RUST_TAD` (via `declare_dependency compile_args`) and links the Rust archive.

### 3. Meson archive linking

`declare_dependency(link_args: [path_to_archive])` is the correct Meson idiom for pre-built static archives that are not themselves Meson `static_library()` targets. The archive path must be absolute (derived from `meson.current_build_dir() / archive_name`). The `sources: [custom_target]` field in the dependency ensures the `custom_target` runs before any link step that consumes the dependency.

### 4. Rust panic=abort is required

Without `panic = "abort"` in the workspace `[profile.release]`, the Rust `staticlib` references `rust_begin_unwind` which requires linking `libstd`. With `panic = "abort"`, the archive is self-contained (depends only on libc which the C linker already brings in). This is essential for embedding Rust in a C shared library.

### 5. VMAF score isolation confirmed

A CPU build with `--feature tad` produces per-frame `tad` and `tad_sad` scores without affecting the standard VMAF score path. The `vmaf_fex_tad` extractor is registered in `feature_extractor_list[]` but is never activated unless explicitly requested via `--feature tad` or a model that references it (no such model exists).

### 6. Netflix golden-data gate unaffected

The TAD extractor is not part of any VMAF model. The three canonical Netflix golden pairs produce the same VMAF scores whether or not `enable_rust_features=true`. Verified by confirming `vmaf_fex_tad` is only invoked when `--feature tad` is present in the feature request list.

## Recipe for future Rust feature extractors

1. Add a crate under `core/src/feature/rust/<name>/` following the TAD template.
2. Add the crate path to the workspace `members` in the root `Cargo.toml`.
3. In `core/src/meson.build`: add a `custom_target` + `declare_dependency` for the new archive (copy the TAD block, change the name).
4. Add a `tad_rust.c`-style C wrapper at `core/src/feature/<name>_rust.c`. Compile it as a direct source of `library('vmaf')` (not into `libvmaf_feature.a`).
5. Register the extractor in `feature_extractor.c` behind a `#if HAVE_<NAME>` guard.
6. Wire the new define through the `declare_dependency compile_args`.

See ADR-0707 for the full rationale and constraint summary.
