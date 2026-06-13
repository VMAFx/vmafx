<!-- markdownlint-disable MD060 -->
# Research — cargo-machete unused-dependency audit (2026-05-30)

## Question

Are there unused Rust dependencies in the VMAFX workspace that
`cargo-machete` (and, where available, `cargo udeps`) can flag for
removal?

## Method

1. Created a sweep worktree against `origin/master` (`544299fae1`).
2. `cargo install cargo-machete` → version `0.9.2`.
3. Ran `cargo-machete` (default mode) at workspace root.
4. Ran `cargo-machete --with-metadata` for the deeper check that
   walks build/transitive metadata.
5. Verified every flagged dep by reading the corresponding crate's
   `build.rs` / `src/` to confirm whether the lint is a true positive
   or a false positive.
6. `cargo udeps` (nightly-only) was attempted but skipped: no `rustup`
   on this host means no nightly toolchain to run it. Documented as
   a follow-up.
7. `cargo build --all` + `cargo test --all` after every Cargo.toml
   edit to confirm no breakage.

## Findings

### Workspace shape

The Rust workspace has two member crates:

| Crate | Path | Purpose | Deps |
|---|---|---|---|
| `vmafx-sys` | `bindings/rust/vmafx-sys/` | Raw FFI bindings to `libvmaf.so` (ADR-0702) | `bindgen` (build-dep) |
| `vmafx-tad` | `core/src/feature/rust/tad/` | TAD extractor pilot, C header via cbindgen (ADR-0707) | `cbindgen` (build-dep) |

Neither crate has any normal (non-build) dependencies. The audit
surface is therefore *very* small.

### `cargo-machete` (default mode)

```text
cargo-machete didn't find any unused dependencies in this directory.
```

True positive: zero unused regular dependencies because there *are*
no regular dependencies.

### `cargo-machete --with-metadata`

```text
vmafx-tad -- ./core/src/feature/rust/tad/Cargo.toml:
    cbindgen
vmafx-sys -- ./bindings/rust/vmafx-sys/Cargo.toml:
    bindgen
```

Both flags are **false positives**. Verified by reading each
`build.rs`:

- `bindings/rust/vmafx-sys/build.rs:30` — `bindgen::Builder::default()`
  call generates the FFI binding from `libvmaf.h`.
- `core/src/feature/rust/tad/build.rs:23` — `cbindgen::Builder::new()`
  call emits the C header that Meson's `custom_target` copies into the
  build tree (consumed by `tad_rust.c`).

This matches the known cargo-machete behaviour: `--with-metadata`
ignores `build.rs` symbol usage. The fix recommended by the tool's
own help text is to add `[package.metadata.cargo-machete] ignored = [...]`.

### Build / test after Cargo.toml edits

```text
$ cargo build --all
    Finished `dev` profile [unoptimized + debuginfo] target(s) in 0.02s
$ cargo test --all
test result: ok. 5 passed; 0 failed; 0 ignored
```

All five TAD unit tests pass; vmafx-sys has no unit tests yet
(scaffolding stage).

### `cargo update --dry-run` (informational)

`cargo update --dry-run` reports the lockfile would re-lock 31 transitive
packages to current upstream tips (no MAJOR bumps within semver ranges).
The vmafx-sys `bindgen = "0.69"` pin would stay; `cargo update` would
emit a hint that `0.72.1` is available behind a SemVer-major bump (not
attempted here — out of scope for this audit; tracked separately by
PR #323 Cargo.lock regen). No security advisories surfaced (the dry-run
doesn't run `cargo audit`; not invoked).

## Decision

Add `[package.metadata.cargo-machete] ignored = [...]` blocks to both
crates with an inline comment citing the build.rs codegen rationale.
This silences the false positive durably (across cargo-machete
upgrades) and keeps the `--with-metadata` audit useful for any future
real unused-dep finding.

See ADR-0904.

## Follow-ups

- Install rustup + a nightly toolchain in the dev container if we
  want `cargo udeps` to become a routine audit. Not blocking — it
  finds a similar class of issue to cargo-machete with a different
  detection method.
- When the Rust workspace grows beyond the FFI + TAD pilots, this
  pattern (build-only crate with `[package.metadata.cargo-machete]`
  ignore) is the canonical convention for any future cbindgen /
  bindgen / proc-macro-style codegen-only dep.

## Sources

- `cargo-machete` upstream README: <https://github.com/bnjbvr/cargo-machete>
- `bindgen` docs: <https://rust-lang.github.io/rust-bindgen/>
- `cbindgen` docs: <https://github.com/mozilla/cbindgen>
- ADR-0702 — vmafx-sys FFI crate.
- ADR-0707 — TAD Rust pilot.
- ADR-0108 — six-deliverables rule (this digest is D1).
