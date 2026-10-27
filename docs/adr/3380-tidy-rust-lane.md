<!-- markdownlint-disable MD013 MD060 -->
# ADR-3380: The `rust` tidy lane measures the sources built with `enable_rust_features`

- **Status**: Accepted
- **Date**: 2026-10-27
- **Deciders**: lusoris
- **Tags**: `ci`, `lint`, `rust`

## Context

[ADR-1142](1142-whole-codebase-standards.md) bounds clang-tidy findings in every
file, and `scripts/ci/check-tidy-coverage.py` refuses a C or C++ file no lane
reads. Nine translation units compile only with
`-Denable_rust_features=true`: the Rust shims `core/src/rust/shim/rust_predict.c`
and `rust_twins.cpp`, `core/src/feature/tad_rust.c` and six tests under
`core/test/` (`test_tad_rust.c`, `test_rust_*.c`). That option's Meson step runs
cargo and cbindgen, the dev image carries neither, so no lane configured it and
the nine files sat in `.config/lint-exceptions.d/clang-tidy-coverage.toml` with
an expiry of 2026-12-31 (issue #2880).

## Decision

We will add an eighth container lane, `rust`, run like the others by
`scripts/dev/tidy-lane.sh` in the digest-pinned dev container and hosted as the
matrix leg `Tidy Lane (rust)` ([ADR-2796](2796-hosted-tidy-all-lanes.md)):

- `tidy-lane.sh` installs `rustup` from the image's distribution when absent and
  the toolchain `build-config.env` pins (`RUST_VERSION`, minimal profile) under
  `/opt/vmafx-rust`; an image that already carries that toolchain skips the
  download, and a toolchain that does not report the pinned version fails the
  lane.
- The lane configures the core with gcc-15, `-Denable_rust_features=true` and
  every GPU backend off, and runs the ratchet only on the nine units
  (`--select` on `core/src/rust/`, `tad_rust.c` and the Rust tests); the `cpu`
  lane keeps measuring everything else.
- The selector `tidy_rust` (`own_paths_only`) routes the leg: `core/src/rust/`,
  `tad_rust.c`, the `core/test/test_*rust*` tests, `build-config.env`,
  `rust-toolchain.toml` and the lane infrastructure. The leg joins the
  aggregator's required lists and the nightly sweep.
- The baseline `scripts/ci/tidy-baseline-rust.json` (32 warnings) is written by
  the lane, and the nine exception entries of #2880 are removed.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Bake the Rust toolchain into `dev/Containerfile` | No download per run | Every image consumer pays for it; a toolchain bump becomes an image rebuild | The lane needs it, the image does not; the lane can take a later image that has it without change |
| Measure the files in the `cpu` lane with Rust enabled | No new leg | Every core change would install Rust and build the crates | Cost on changes that do not touch these files |
| Reuse the `rust-ci.yml` job's runner setup | Toolchain already installed there | Outside the pinned container every other lane measures in; another clang-tidy version | One measuring environment for all container lanes |
| Keep renewing the exception entries | No work | Exceptions expire by design | ADR-1142 binds every file |

## Consequences

- **Positive**: a change to the Rust shims, `tad_rust.c` or the Rust tests is
  checked by clang-tidy before merge; `check-tidy-coverage.py` needs no entry
  for them.
- **Negative**: a selected leg downloads the pinned toolchain and builds the
  crates, a few minutes more than the `cpu` lane.
- **Neutral / follow-ups**: a new C or C++ unit built only with the option needs
  its path in the lane's `--select` list in the `Makefile`, or
  `check-tidy-coverage.py` refuses it.

## References

- Issue #2880; [ADR-1142](1142-whole-codebase-standards.md),
  [ADR-1471](1471-tidy-lanes-in-dev-container.md),
  [ADR-1700](1700-tester-selectors-own-paths-only.md),
  [ADR-2796](2796-hosted-tidy-all-lanes.md),
  [ADR-3379](3379-tidy-integrations-lane.md).
