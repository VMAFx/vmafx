<!-- markdownlint-disable MD013 MD060 -->
# ADR-0706: Rust `vmafx-sys` FFI bindings crate

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: lusoris
- **Tags**: `rust`, `bindings`, `ffi`, `build`

## Context

Phase 4 of the VMAFX modernisation plan introduces Rust as a first-class language in
the repository. The initial deliverable is a low-level FFI crate (`vmafx-sys`) that
exposes the libvmaf C API to Rust callers.

The C API surface is stable and header-documented; manually writing bindings would
create drift risk every time a public header changes. A build-time code-generation
approach avoids that risk entirely.

The crate must satisfy two downstream consumers: a forthcoming higher-level `vmafx`
crate (Phase 4 follow-on) and a Rust-based feature extractor pilot (sibling PR).

## Decision

We introduce `bindings/rust/vmafx-sys` with the following design:

- `build.rs` uses **bindgen 0.69** to parse `libvmaf.h` (and its transitive
  includes) at build time and emit `bindings.rs` into `$OUT_DIR`.
- `src/lib.rs` re-exports the generated bindings and includes `src/safe.rs`.
- `src/safe.rs` provides a thin safe wrapper layer (`VmafContext`, `VmafModel`,
  picture helpers) that confines `unsafe` to the FFI call sites and returns
  `Result<T, VmafxError>`.
- The install path is read from `LIBVMAF_PREFIX` (default `/usr/local`); no
  `pkg-config` dependency is introduced to keep the build portable.
- A Rust workspace `Cargo.toml` is added at the repo root with `resolver = "2"`.
- CI runs `cargo fmt --check` + `cargo clippy -D warnings` + `cargo test` on
  every PR touching `bindings/rust/`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|--------|------|------|----------------|
| Hand-written bindings | No bindgen dep | Drift risk; maintenance burden per header change | Rejected — 15+ public functions in libvmaf.h; drift is certain |
| `cc` crate only (no bindgen) | Simpler build script | Still requires hand-written `extern "C"` blocks | Same drift problem as hand-written |
| bindgen 0.72 (latest) | Newer feature set | Introduced breaking API changes after 0.70; crate ecosystem not fully caught up | Conservative choice; upgrade to 0.72 in a follow-on PR |
| `cbindgen` (inverse direction) | Auto-generates C from Rust | Wrong direction — we are wrapping C, not exposing Rust to C | Not applicable |

## Consequences

- **Positive**: Rust callers get idiomatic access to libvmaf with no manual
  binding maintenance. The safe layer gives a `Result`-based API that works
  naturally with `?`.
- **Positive**: Future crates (`vmafx`, feature-extractor pilot) can depend
  on `vmafx-sys` without re-implementing the FFI surface.
- **Negative**: `libclang` must be present on every developer machine and CI
  runner that builds `vmafx-sys`. This is standard on Linux; macOS / Windows
  need minor setup (documented in `docs/development/rust.md`).
- **Neutral**: The safe layer wraps only the scoring and picture pipeline; raw
  FFI is still the escape hatch for features not yet wrapped (model collections,
  feature-level scores, output writing). The safe layer can be extended
  incrementally.

## References

- Phase 4 umbrella: `feat/vmafx-phase4-language-modernization-foundation`
- ADR-0701: VMAFX cloud-native redesign (Phase 3 / Phase 4 context).
- `docs/development/rust.md` (this PR).
- bindgen crate: <https://docs.rs/bindgen/0.69>.
- req: "Rust pilot: `vmafx-sys` FFI bindings crate" (2026-05-28 session).
