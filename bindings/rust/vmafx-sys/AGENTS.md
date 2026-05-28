# AGENTS.md — vmafx-sys

## Rebase-sensitive invariants

- **bindgen allowlist**: `build.rs` uses `allowlist_function("vmaf_.*")`, `allowlist_type("Vmaf.*")`, and `allowlist_var("VMAF_.*")`. Do not widen this to `.*` — it would pull in OS internal types and break compilation. Widening requires an ADR.
- **safe layer unsafe boundary**: `unsafe` must remain confined to the FFI call sites inside `src/safe.rs`. Adding `unsafe` to safe-layer function signatures breaks the contract of the module.
- **`links = "vmaf"`**: The `links` field in `Cargo.toml` tells Cargo this crate provides the native `vmaf` library. Only one crate in a build graph may set `links = "vmaf"`. Do not add a second crate with the same links key.
- **No `pkg-config` dependency**: `build.rs` locates libvmaf via `LIBVMAF_PREFIX` only. This is intentional — `pkg-config` is not universally available (cross-compile targets, minimal CI containers). Do not add a `pkg-config` dep without an ADR.
- **Test data paths**: integration tests and examples resolve YUV/model paths relative to `VMAFX_REPO` (or auto-detected from `CARGO_MANIFEST_DIR`). Never hardcode absolute paths into test source.
