<!-- markdownlint-disable MD013 MD060 -->
# ADR-3379: The `integrations` tidy lane measures the GStreamer element and the FFmpeg filter sources

- **Status**: Accepted
- **Date**: 2026-10-27
- **Deciders**: lusoris
- **Tags**: `ci`, `lint`, `gstreamer`, `ffmpeg`

## Context

[ADR-1142](1142-whole-codebase-standards.md) bounds clang-tidy findings in every
file, and `scripts/ci/check-tidy-coverage.py` refuses a C file no lane reads.
The GStreamer element (`gstreamer/src/*.c`) and the FFmpeg sources kept in
`ffmpeg-patches/src/` (`vf_vmafx.c`, `vf_vmafx_pre.c`, `ffmpeg_dec_hwaccel.c`)
are built by neither the core Meson build nor any lane, so they landed on
`master` with category C entries in
`.config/lint-exceptions.d/clang-tidy-coverage.toml` (issue #2860). Both trees
need their consumer's headers: the element is a Meson project of its own that
needs the GStreamer development packages, and the FFmpeg sources only compile
inside a configured FFmpeg tree with the patch series applied.

## Decision

We will add a seventh container lane, `integrations`, run like the others by
`scripts/dev/tidy-lane.sh` in the digest-pinned dev container and hosted as the
matrix leg `Tidy Lane (integrations)` ([ADR-2796](2796-hosted-tidy-all-lanes.md)):

- `scripts/ci/tidy-integrations-build.sh` builds, next to the core build, the
  element with `-Dcuda=enabled -Dvulkan=enabled` (so the optional bodies are
  measured, not their stubs) and the patched FFmpeg tree the patch-stack gate
  replays (`scripts/ci/ffmpeg_patch_stack.py --check --keep-tree`), configured
  with libvmafx, CUDA, Vulkan and libdrm; it fails when a required `CONFIG_`
  flag is off, and builds only the three objects from `ffmpeg-patches/src/`
  with `make V=1`.
- `scripts/ci/gen-integrations-compile-commands.py` adds the element's entries
  and the FFmpeg compile lines, pointed back at the repository copies, to the
  core compile database; a missing input or source exits 1, so the lane cannot
  go clean by measuring nothing.
- `tidy-lane.sh` installs the GStreamer development packages in the container
  when they are absent.
- The selector `tidy_integrations` (`own_paths_only`) routes the leg:
  `gstreamer/`, `ffmpeg-patches/src/`, the series and its patches,
  `build-config.env` and the lane infrastructure. The leg joins the
  aggregator's required lists and the nightly sweep.
- The baseline `scripts/ci/tidy-baseline-integrations.json` is written by the
  lane, and the exception entries of #2860 are removed.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Measure the files in the `cpu` lane | No new leg | The `cpu` lane would need the GStreamer packages and an FFmpeg build on every core change | Cost on changes that do not touch these files |
| Two lanes, one per integration | Narrower routing | Two image pulls; `build-config.env` and the lane scripts select both anyway | One lane is enough for three files plus one plugin |
| A hand-written compile database with fixed flags | No FFmpeg configure | Drifts from the flags FFmpeg actually compiles with; misses `config.h` | The lane must read the code as the consumer builds it |
| Keep renewing the exception entries | No work | Exceptions expire by design (category C, 10 days) | ADR-1142 binds every file |

## Consequences

- **Positive**: a change to the element or the filter sources is checked by
  clang-tidy before merge; `check-tidy-coverage.py` needs no entry for them.
- **Negative**: a selected leg configures FFmpeg and builds the plugin on top of
  the core build, a few minutes more than the other container lanes.
- **Neutral / follow-ups**: a new `.c` file under `ffmpeg-patches/src/` needs a
  row in `FFMPEG_UNITS` of the generator and an object in the build script; the
  generator fails until both exist.

## References

- Issue #2860; [ADR-1142](1142-whole-codebase-standards.md),
  [ADR-1471](1471-tidy-lanes-in-dev-container.md),
  [ADR-1700](1700-tester-selectors-own-paths-only.md),
  [ADR-2796](2796-hosted-tidy-all-lanes.md),
  [ADR-1240](1240-ffmpeg-release-patch-lifecycle.md).
