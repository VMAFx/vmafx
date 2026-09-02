<!-- markdownlint-disable MD013 MD060 -->
# ADR-0904: Pin `cargo-machete` ignore entries for `bindgen` / `cbindgen` build dependencies

- **Status**: Accepted
- **Date**: 2026-05-30
- **Deciders**: Lusoris
- **Tags**: rust, build, ci, workspace

## Context

`cargo-machete` is the Rust ecosystem's lint for unused crate-level
dependencies. We added it to the periodic audit rotation alongside
`cargo udeps` to keep the two-crate Rust workspace (`vmafx-sys`,
`vmafx-tad`) tidy as more Rust pilots land (ADR-0702, ADR-0707).

The default `cargo-machete` invocation (no `--with-metadata`) reports
zero unused dependencies and matches reality: every direct dep is
referenced somewhere in source. The `--with-metadata` mode, however,
falsely flags two **build-only** dependencies:

- `bindgen` in `bindings/rust/vmafx-sys/Cargo.toml` — consumed by
  `build.rs` to generate FFI bindings against `libvmaf.h`.
- `cbindgen` in `core/src/feature/rust/tad/Cargo.toml` — consumed by
  `build.rs` to emit the C header that the Meson `custom_target` in
  `core/src/meson.build` copies into the build tree.

This is a known limitation: `--with-metadata` does not introspect
`build.rs` symbol references the same way it walks normal library /
binary sources. Without a pinned ignore, every future audit (and any
CI gate we later wire) will surface the same two false positives and
risk a maintainer "fixing" them by deleting the dep — which silently
breaks codegen.

## Decision

We add a `[package.metadata.cargo-machete]` section with
`ignored = [...]` to each of the two affected crates, citing the
`build.rs` usage in a comment. The ignore is scoped strictly to the
build-only crates that `--with-metadata` mis-classifies; non-build
dependencies remain subject to the normal lint.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Pin `[package.metadata.cargo-machete] ignored = [...]` per crate (chosen) | Survives across `cargo-machete` upgrades; local to the affected crate; documented inline | Two extra Cargo.toml blocks | — |
| Always invoke `cargo-machete` without `--with-metadata` | No Cargo.toml churn | `--with-metadata` catches a wider class of false-negative cases for regular library deps; we want it on for future Rust crates | Loses lint coverage |
| Wrap `cargo-machete` in a project script that greps build.rs first | Fully automatic | Reimplements upstream's ignore mechanism; more code to maintain | Reinvents the wheel |
| Move codegen out of `build.rs` into a checked-in generated file | Eliminates the build-dep entirely | Loses regeneration on header change; ADR-0702 / ADR-0707 explicitly chose codegen | Conflicts with prior ADRs |

## Consequences

- **Positive**: future `cargo-machete --with-metadata` runs stay
  green on the existing workspace, so any subsequent unused-dep
  finding is real and worth investigating.
- **Negative**: maintainers adding a new build-only dep to either
  crate must remember to extend the `ignored = [...]` list; the
  inline comment flags this convention.
- **Neutral / follow-ups**: when the Rust workspace grows beyond
  the FFI + TAD pilots, this convention extends to any new crate
  whose only `build-dependencies` entry is a codegen tool consumed
  by `build.rs`.

## References

- ADR-0702 — `vmafx-sys` raw FFI bindings crate.
- ADR-0707 — TAD Rust pilot using `cbindgen` for C header generation.
- `cargo-machete` upstream behaviour: <https://github.com/bnjbvr/cargo-machete>
- Source: agent dispatch brief (cargo-machete + cargo-udeps unused-deps
  sweep, 2026-05-30).
