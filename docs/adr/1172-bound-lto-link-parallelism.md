<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1172: Bound per-link LTO parallelism to four partitions by default

- **Status**: Accepted
- **Date**: 2026-09-04
- **Deciders**: Lusoris (maintainer), Claude Code session d0961a83
- **Tags**: `build`, `meson`, `developer-experience`, `lto`

## Context

`core/meson.build` enables link-time optimisation for every build through
`default_options: ['b_lto=true']`. Meson passes plain `-flto` to GCC, and since
GCC 10 plain `-flto` means `-flto=auto`: each link runs one `lto1-ltrans` job per
core unless a GNU make jobserver is present, which Ninja does not provide. With
`ninja -j 8` and the project's dozens of link targets (the shared and static
library, the CLI, the sidecar tools, every C unit test), several links overlap
and each spawns 32 partitions on the maintainer's 32-core workstation. Measured on
2026-09-04 while three agent builds linked at once: 1-minute load 190 on a
machine whose working limit is 32, nine `lto1-ltrans` processes above 700 % CPU,
the interactive session unusable. `docs/development/build-flags.md` also claimed
`b_lto` defaults to `false`, which is wrong and hid the cause.

## Decision

We keep LTO on by default and add `b_lto_threads=4` to the project
`default_options`. Meson turns it into `-flto=4` for GCC and `-flto-jobs=4` for
Clang, so a link uses at most four partitions; `ninja -j N` then bounds a build at
`4N` LTO jobs. Anyone with a dedicated build box can raise it per build directory
(`-Db_lto_threads=0` restores the compiler default). The build-flags page states
the real defaults for both options.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| `b_lto=false` by default | No LTO cost at all | Loses the measured scalar / AVX2 speedup on the shipped binaries and diverges the dev build from the release build | Performance is a release property; ADR-1102 wants dev and container builds alike |
| Leave defaults, tell agents to pass `-Db_lto_threads` | No tree change | Every brief, skill and human must remember it; the storm recurs on the first forgotten build | A default that hurts by default is the bug |
| Rely on `ninja --jobserver` (Ninja ≥ 1.13) | Compiler and Ninja share one token pool | Not in the pinned toolchains yet; GCC only honours it when the jobserver is in `MAKEFLAGS` | Revisit when the pinned Ninja ships it |
| `b_lto_threads=4` in `default_options` (chosen) | One line, keeps LTO, bounds load, overridable per build dir | Slightly longer links on a dedicated many-core box | — |

## Consequences

- **Positive**: a full agent build no longer takes the workstation down; three
  parallel builds stay under the 32-load budget; release binaries keep LTO.
- **Negative**: link phases on a many-core CI runner are a little slower than
  unbounded; CI can override the option if that ever matters.
- **Neutral / follow-ups**: `docs/development/build-flags.md` corrected; the
  container build inherits the default; no ffmpeg-patch impact (build flags of
  libvmaf only).

## References

- `core/meson.build` `default_options`; `docs/development/build-flags.md`.
- Meson `GnuLikeCompiler.get_lto_link_args` (installed meson 1.12.0): `-flto=<threads>` when `b_lto_threads` > 0.
- req — maintainer's standing workstation budget (paraphrased): keep the machine's load at or below the core count, it is the daily driver.
