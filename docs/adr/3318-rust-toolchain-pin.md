<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3318: One pinned Rust toolchain for every Rust build

- **Status**: Accepted
- **Date**: 2026-10-15
- **Deciders**: Lusoris (issue #2568, Rust core P0a)
- **Tags**: build, rust, ci, renovate, supply-chain, container

## Context

The Rust extractors (ADR-1713) were built by three compilers. The `Rust`
workflow installed `toolchain: stable`, the version of the day. Developer hosts
used whatever rustup had as its default. The dev container had no Rust at all,
and ADR-1102 makes the container the only place a publishable artifact comes
from. Inside it, `-Denable_rust_features=true` only warned that cargo was
missing and built the C extractors, so a container build asked for the Rust
path silently returned an artifact without it (state row
`T-RUST-DEV-CONTAINER-TOOLCHAIN-2026-10-05`). The workspaces declared
`rust-version = "1.85"`, the edition 2024 floor, not the version anything was
built with.

Issue #2568 asks for one pin in `build-config.env`, managed by Renovate, with
`rust-toolchain.toml`, every crate's `rust-version`, the container and one CI
lane on that version, and a configure error without cargo. The issue named
Rust 1.98.1. Rust 1.99.0 had been released when the work started; the fleet
rule to use the newest working version picks 1.99.0. Both workspaces build and
pass clippy on it.

## Decision

- `build-config.env` owns `RUST_VERSION="1.99.0"`.
- `rust-toolchain.toml` at the root pins the same channel with the `rustfmt`
  and `clippy` components and the minimal profile, so rustup selects it for
  every `cargo` call inside the checkout.
- Both workspaces (`Cargo.toml`, `core/src/rust/Cargo.toml`) declare
  `rust-version = "1.99.0"` and every crate inherits it
  (`rust-version.workspace = true`); an older cargo refuses the build by name.
- `core/src/rust/build_staticlib.py` runs cargo in the manifest's directory:
  Meson runs it in the build directory, where rustup would not see
  `rust-toolchain.toml` and would pick its default toolchain.
- `core/src/meson.build` stops with `error()` when `enable_rust_features=true`
  and no cargo is found.
- `dev/Containerfile` installs rustup-init 1.29.1, checked against its
  published SHA-256, with `--default-toolchain "${RUST_VERSION}"` read from
  `build-config.env`, under `/opt/rust`; it verifies `rustc --version`. The
  `libvmaf-compile` stage configures with `-Denable_rust_features=true` and
  fails when the Rust archive is missing after the build.
- `.github/workflows/rust-ci.yml` installs `toolchain: 1.99.0`.
- `scripts/ci/check-workflow-versions.py` (`check_rust_pins()`) refuses any of
  these copies that differs from `RUST_VERSION`, and a literal version in the
  container.
- Renovate: one `custom.regex` manager (datasource `rust-version`, versioning
  `rust-release-channel`) reads all five pins as one dependency in the group
  `Rust toolchain` without automerge; the built-in `rust-toolchain` manager is
  disabled so it cannot raise one file alone.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| `build-config.env` owns the pin, checked copies, one Renovate group (chosen) | Same shape as the other SDK pins; one pull request per Rust release; the gate refuses drift | Five copies to keep in step | — |
| `rust-toolchain.toml` as the only pin, read by the container and the workflow at run time | One literal | The container sources `build-config.env` for every SDK; a second config format breaks HISS-19; the workflow action takes the version as an input | Two pin sources for one repository |
| Keep `stable` everywhere | No pin to update | Builds change compiler without a commit; RC4 exactness evidence is not reproducible | Defeats the issue |
| Pin 1.98.1 as the issue states | Matches the issue text | Older than the current release when the work started | Fleet rule: newest working version |
| Keep the warning when cargo is missing | Hosts without Rust still configure | A requested Rust build returns a C-only artifact without notice | No silent fallback |

## Consequences

- A container build with `-Denable_rust_features=true` contains the Rust
  extractors; a build without cargo fails at configure with a message naming
  the pinned version.
- A host without rustup but with an older system cargo fails the build on
  `rust-version`; rustup users get the pinned toolchain on the first `cargo`
  call in the checkout.
- A Rust release opens one Renovate pull request that changes all five pins;
  it is merged by a person, because a compiler change can move scores.
- The dev image grows by the minimal Rust toolchain.

## Supply-chain impact

The container downloads `rustup-init` from `static.rust-lang.org` and checks
its SHA-256 before running it; rustup then downloads the pinned toolchain,
whose components rustup verifies against the channel manifest. No Rust code
reaches a published artifact that the C build did not already carry when the
features are on; the archive itself has no external crates (`--offline
--locked`).

### SBOM delta

The dev image gains the Rust toolchain (rustc, cargo, rust-std, rustfmt,
clippy 1.99.0). Library artifacts built in it gain the Rust extractor archive
`libvmafx_core_rs.a`, linked into `libvmaf`.

### Carbon / footprint

One toolchain download per image build, cached in its layer; Rust builds stop
running twice when a toolchain drift breaks one of them.

## References

- Issue #2568 (Rust core P0a), parent #2567; RC4 #1723.
- State row `T-RUST-DEV-CONTAINER-TOOLCHAIN-2026-10-05`.
- [ADR-1713](1713-rc4-rust-extractor-framework.md): the Rust extractor archive.
- [ADR-1102](1102-phase4b9-container-only-publishing.md): the container is the
  only source of published artifacts.
- [ADR-3312](3312-renovate-python-ci-version-group.md): the same Renovate shape
  for the CI Python interpreter.
- rustup toolchain file: <https://rust-lang.github.io/rustup/overrides.html#the-toolchain-file>
- Renovate rust-version datasource: <https://docs.renovatebot.com/modules/datasource/rust-version/>
