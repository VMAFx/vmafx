<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-2783: The patched FFmpeg is built with MSVC against vmaf.lib in CI

- **Status**: Accepted
- **Date**: 2026-10-08
- **Deciders**: lusoris
- **Tags**: ci, windows, ffmpeg, upstream-port

## Context

The FFmpeg checks of `ffmpeg-integration.yml` build the configured FFmpeg
release (`FFMPEG_TAG` in `build-config.env`) with GCC on Ubuntu, clang on
macOS, and GCC with the whole patch series against a SYCL build. No check
built FFmpeg with the MSVC compiler, so nothing showed that the patch series
compiles with `cl.exe` or that FFmpeg links the static libraries an MSVC build
installs (`vmaf.lib`, `vmafx.lib`, ADR-2752).

Upstream Netflix/vmaf `3e1385bed` adds a Windows row to its FFmpeg job. It
builds FFmpeg's `master` branch through a Meson port of FFmpeg maintained
outside the FFmpeg project, renames the static libraries it links, and runs a
score check that may fail without failing the job (`continue-on-error`).

The developer smoke script `ffmpeg-patches/test/build-and-run.sh` already
builds the configured release with the whole series and runs FFmpeg's local
test subset, but only with the host's `cc` and outside CI.

Every object of one MSVC link must use the same C runtime library. A Meson
release build compiles with `/MD` (`b_vscrt=from_buildtype`); FFmpeg's MSVC
toolchain sets no runtime option of its own.

## Decision

We will add the required check `FFmpeg Windows MSVC` (`ffmpeg-msvc-work` and
`ffmpeg-msvc-gate` in `ffmpeg-integration.yml`, routed by the `c_core`
selector like the other FFmpeg checks). On `windows-2025` it:

1. builds and installs a static MSVC build of the commit and checks the
   library names (`check_msvc_library_names.py`);
2. runs `ffmpeg-patches/test/build-and-run.sh` in an MSYS2 UCRT64 shell that
   inherits the MSVC environment, with `FFMPEG_TOOLCHAIN=msvc` (FFmpeg's own
   `configure --toolchain=msvc`, `-MD` for C and C++) and
   `VMAF_SCORE_CHECK=1`.

The script's warning gate keeps refusing every warning on the other
toolchains. Under `msvc` it refuses every linker warning and every compiler
warning on a line the patch series adds or changes (the `+` side of
`git diff -U0` between the release and the patched head), matched by path and
line. The first run of the check found about 400 `cl.exe` warnings in
FFmpeg's own sources at the configured release (`C4334`, `C4113`, `C5287`,
`C5286` and others) and `D9024` from FFmpeg's host-tool links, which the
series does not write, and two in the series itself: `vf_vmaf_pre.c` and
`vf_libvmaf_tune.c` passed their `enum AVPixelFormat` lists to
`ff_set_common_formats_from_list2()`, which takes `const int *` (`C4133`).
Patches 0002 and 0008 now call `ff_set_pixel_formats_from_list2()`, the
typed form the release declares beside it. Some of FFmpeg's own warnings sit
in files the series also edits (patch 0019 touches `libavcodec/vlc.c`, where
`cl.exe` reports `C4334` on three lines the patch leaves alone), so a
file-level scope still failed; the line-level scope passes on that run and
still reports the four `C4133` lines of the earlier one. A warning the series
causes on a line it does not write is left to the GCC and clang checks, which
refuse every warning.

The script gains four settings, all checked before anything is fetched:
`FFMPEG_TOOLCHAIN`, `FFMPEG_JOBS`, `SMOKE_FATE` and `VMAF_SCORE_CHECK`. The
score check takes its scoring graph, CLI run and exact-text comparison from
`scripts/ci/upstream-consumer-lib.sh`; the graph builder moves there from
`upstream-ffmpeg-compat.sh` so both use one definition.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| FFmpeg's configure under MSYS2 through the smoke script (chosen) | The build system the patch series patches and distributors use; the same command runs on a developer's Windows machine; one implementation of the series replay | MSYS2 adds a few minutes of setup | — |
| Upstream's Meson port of FFmpeg | No MSYS2 | A second build description of FFmpeg, maintained outside FFmpeg and not at the configured tag; the series patches `configure`, which the port does not read | It would not build what the patches change |
| A new inline workflow script | Self-contained | A fourth copy of the series replay next to the SYCL leg, the container and the smoke script (HISS-19) | Reuse the smoke script |
| Build VMAFx with `-Db_vscrt=mt` instead of FFmpeg with `-MD` | FFmpeg needs no extra flag | Departs from Meson's default, which every other recipe uses | Change the consumer, document the rule |
| Refuse every `cl.exe` warning, as on the other toolchains | One rule for every toolchain | Fails on about 400 warnings in FFmpeg's own sources that no VMAFx change can remove | Scope the gate to the series' lines and the linker |
| Refuse every `cl.exe` warning in a file the series edits | Catches a warning the series causes elsewhere in its files | Fails on FFmpeg's own warnings in files patch 0019 edits (`libavcodec/vlc.c`) | The GCC and clang checks refuse every warning; the MSVC gate reads the series' own lines |
| Advisory check (`continue-on-error`, as upstream's score step) | Cannot block a merge | A failing consumer would read as passing | Required, as the other FFmpeg checks (ADR-1297) |

## Consequences

- **Positive**: an MSVC portability break in the patch series, in the public
  headers or in the library names fails a required check; the filter's scores
  must equal the CLI's as text.
- **Negative**: one more Windows job (about an hour with FFmpeg's test
  subset) on every change routed to the C library or the patches.
- **Neutral / follow-ups**: the job builds x64 only; an ARM64 MSVC FFmpeg
  build would need FFmpeg's `armasm64` path.

## References

- Q-300 (maintainer, 2026-10-08): "D3: yes, an MSVC CI leg building our
  configured FFmpeg release + full patch series against vmaf.lib (after D2)."
- Upstream Netflix/vmaf `3e1385bed` ("CI: Add build and test FFmpeg-libvmaf
  on MSVC").
- [ADR-2752](2752-msvc-static-library-platform-names.md) — `vmaf.lib` and
  `vmafx.lib`.
- [ADR-1297](1297-ci-gate-every-reporting-check.md) — the FFmpeg checks
  are required.
- Microsoft C++ documentation, "/MD, /MT, /LD (Use run-time library)": all
  modules passed to one linker invocation use the same runtime option.
