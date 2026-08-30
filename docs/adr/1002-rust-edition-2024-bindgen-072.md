# ADR-1002: Bump Rust workspace to edition 2024 and bindgen to 0.72

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `rust`, `build`, `workspace`

## Context

PR #592 deferred the Rust workspace edition bump from 2021 to 2024 because
bindgen 0.69 emits bare `extern "C"` blocks. Rust edition 2024 promotes the
`unsafe_op_in_unsafe_fn` lint from warn to deny and also requires `unsafe
extern "C"` for external-function declarations — meaning bindgen 0.69-generated
code would fail to compile in edition 2024 crates that include it.

bindgen 0.70+ emits `unsafe extern "C"` by default, making it compatible with
edition 2024. The latest stable release (0.72.1, Q2 2026) is available and adds
no breaking API changes relative to 0.69 for our usage pattern (Builder +
`CargoCallbacks::new()`).

Additionally, the hand-written FFI code in `core/src/feature/rust/tad/src/lib.rs`
used bare `extern "C"` for an FFI import and relied on the implicit safety of
`unsafe fn` bodies — both of which are hard errors in edition 2024.

## Decision

We will bump bindgen to 0.72.1 in `vmafx-sys/Cargo.toml`, set `edition = "2024"`
in the workspace root `[workspace.package]` block, migrate `vmafx/Cargo.toml` to
inherit the edition from the workspace, and fix all edition-2024 incompatibilities
in `vmafx-tad/src/lib.rs`:

- Change bare `extern "C"` import block to `unsafe extern "C"`.
- Change `#[no_mangle]` to `#[unsafe(no_mangle)]` on the three exported functions.
- Wrap all raw-pointer dereferences and unsafe function calls inside explicit
  `unsafe { }` blocks inside `unsafe fn` bodies.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Stay on edition 2021 indefinitely | No migration work | Misses `unsafe_op_in_unsafe_fn` improvements; blocks future edition-2024 features (RPIT lifetime capture, `impl Trait` in `let` etc.) | Edition 2024 is now stable; deferring further adds tech debt |
| Pin bindgen via `#[allow(unsafe_op_in_unsafe_fn)]` suppression | Minimal change | Suppression silences a valuable lint; semantics of the generated code remain edition-2021 even in an edition-2024 crate | Treating a safety lint as noise is not acceptable under ADR-0141 |
| Vendor bindgen output and skip build-time generation | Avoids bindgen dep entirely | Generated file would drift from headers; large committed blob | Contra our "generate at build time" policy in ADR-0702 |

## Consequences

- **Positive**: Rust edition 2024 is active across the whole workspace; future
  edition-2024 language features (RPIT lifetime capture, `gen` blocks, etc.)
  are available without per-crate opt-in; `unsafe_op_in_unsafe_fn` is now a
  hard error, making unsafe-code audits more rigorous; bindgen 0.72 brings
  improved C++ support and upstream bug fixes.
- **Negative**: None expected; the diff is mechanical and all 21 tests pass.
- **Neutral / follow-ups**: `vmafx/Cargo.toml` now inherits `edition` from the
  workspace; any future per-crate edition override must be explicit.

## Supply-chain impact

- **Updated**: `bindgen` `0.69.5` → `0.72.1` (build dependency of `vmafx-sys`).
  License: MIT OR Apache-2.0. Upstream: <https://github.com/rust-lang/rust-bindgen>.
- **Updated**: `rustc-hash` `1.1.0` → `2.1.2` (transitive dep of bindgen). Same
  license family.
- **Removed**: the previous `bindgen 0.69.5` entry from `Cargo.lock`.
- No new network fetches at build time beyond the crates.io registry update.
- No CVE surface change — bindgen is a build-time code-generation tool that
  produces no runtime artifact.

## References

- PR #592 (deferred this bump — cited as the reason for deferral).
- ADR-0702 (vmafx-sys FFI crate — bindgen policy).
- ADR-0707 (TAD Rust pilot — `#[no_mangle]` and `extern "C"` usage).
- ADR-0141 (touched-file cleanup rule — no NOLINT without justification).
- Rust edition 2024 migration guide:
  <https://doc.rust-lang.org/edition-guide/rust-2024/unsafe-op-in-unsafe-fn.html>
- req: PR #592 description ("bindgen 0.69 emits bare `extern "C"` blocks which
  require `unsafe extern` in edition 2024").
