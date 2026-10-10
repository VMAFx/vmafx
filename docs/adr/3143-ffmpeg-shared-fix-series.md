<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3143: VMAFx consumes the shared FFmpeg fix series

- **Status**: Accepted
- **Date**: 2026-10-10
- **Deciders**: Lusoris (operator decision ON-12)
- **Tags**: build, ci, docker, ffmpeg, supply-chain

## Context

`ffmpeg-patches/` carried two kinds of patch against the pinned FFmpeg release:
the VMAFx integration (filters, backend selectors, options) and fixes to FFmpeg
itself, of which one remained, `0019`, the GCC 14/16 diagnostics hardening.
Fixes to FFmpeg are not specific to VMAFx; Pelorus carried its own copies. The
fix patches therefore moved to a repository of their own,
[VMAFx/ffmpeg-patches](https://github.com/VMAFx/ffmpeg-patches) (Q-340), which
publishes a signed release tarball per FFmpeg base: `series.txt`, `patches/`,
and `base.env` naming the FFmpeg tag and commit the series applies to.

Release `v0.1.0-rc.1` targets `n9.0.2`, the release VMAFx pins, and holds four
patches: `0001` (Vulkan queue family, `libavutil/vulkan.c`), `0002` (the
diagnostics hardening, byte for byte the changes of our `0019`), `0003` and
`0004` (NVENC user data SEI, `libavcodec/nvenc.c` and `nvenc.h`). The operator
decided to switch VMAFx to that series now, during the 1.0 freeze (ON-12,
Q-349), instead of after the release.

## Decision

Every VMAFx FFmpeg build applies the shared fix series first and
`ffmpeg-patches/series.txt` second. `build-config.env` pins the series with
`FFMPEG_FIX_SERIES_REPO`, `FFMPEG_FIX_SERIES_TAG` and
`FFMPEG_FIX_SERIES_SHA256`. One script, `scripts/ci/ffmpeg-shared-series.sh`,
downloads the release tarball, refuses any other sha256, refuses a series whose
`base.env` names another FFmpeg tag or commit than `FFMPEG_TAG` /
`FFMPEG_COMMIT`, checks the release signature with cosign where cosign is
installed (required in the `FFmpeg Patch Stack` gate), and applies the series
in its `series.txt` order. The patch-stack replay, the smoke build, the hosted
FFmpeg jobs and the four container builds call that script. `0019` leaves
`ffmpeg-patches/`; the number is retired.

Three of the four patches change how FFmpeg behaves in every VMAFx build:

- `0001`: on a Vulkan device with one queue family, a frame barrier no longer
  releases a frame to `VK_QUEUE_FAMILY_IGNORED`.
- `0003`: `hevc_nvenc` with `udu_sei=1` drops a user data unregistered SEI
  that does not fit NVENC's 1024-byte limit for non-VCL NAL units, with a
  warning, where it used to stop the encode with an out-of-memory error.
- `0004`: `h264_nvenc` and `hevc_nvenc` do not write a user data unregistered
  SEI that NVENC would write truncated; the first one is a warning.

`0002` changes nothing for VMAFx: the same changes were already applied as
`0019`. The operator approved taking the behaviour changes during the freeze.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep `0019` here until after 1.0 | No change during the freeze | Two copies of the diagnostics patch to keep equal; VMAFx builds lack the Vulkan and NVENC fixes Pelorus ships | The operator decided to switch now (ON-12) |
| Vendor the series' patches into `ffmpeg-patches/` | No download at build time | A second copy that drifts from the shared repository, which is what the move ended | One owner per patch |
| Git submodule of VMAFx/ffmpeg-patches | Pinned by commit | No release signature; every checkout and sparse checkout must init it; the Docker contexts would need it too | A verified tarball needs only curl and a sha256 tool |
| Apply with the tarball's own `scripts/series.py` | The provider's tool | Needs Python 3.11 in every builder stage and in the MSYS2 leg | `git am --3way` in `series.txt` order is the documented alternative |
| Pin by tag only | Shorter configuration | A moved tag or a replaced asset would be applied unseen | HISS-11: inputs are pinned by content |

## Consequences

- **Positive**: one owner for each FFmpeg fix; VMAFx and Pelorus build the same
  fixed FFmpeg; the series is verified (sha256 always, signature in the gate)
  before a line of it is applied; a build cannot skip it, because every
  application site calls the same script and a contract test pins the sites.
- **Negative**: FFmpeg builds download a release asset from GitHub (62 kB); a
  new FFmpeg release needs a series release for that base first. The scheduled
  `--refresh --latest` therefore stops with that message and keeps its rebased
  candidates as diagnostics; it no longer proposes a tag move on its own.
- **Neutral / follow-ups**: moving to a new FFmpeg release = pin a series
  release for it together with `FFMPEG_TAG` / `FFMPEG_COMMIT`, then `--refresh`.
  The pin moves to the final `v0.1.0` when it is published.
  `LICENSES/GPL-2.0-or-later.txt` leaves the tree with `0019`, the only file
  under that licence: REUSE refuses a licence text no file uses. The images'
  licence labels are unchanged, because the FFmpeg they contain is.

## Supply-chain impact

- **Build-time fetches**: `ffmpeg-patches-<tag>.tar.gz` from the releases of
  `FFMPEG_FIX_SERIES_REPO`, verified against `FFMPEG_FIX_SERIES_SHA256`; with
  cosign, `SHA256SUMS` and its Sigstore bundle, verified against the identity
  of the repository's `release-build.yml` at the tag.
- **New dependencies**: `curl` in the node image's FFmpeg builder stage (build
  only). cosign in the `FFmpeg Patch Stack` workflow (pinned installer action,
  already used by the publish workflows).

## References

- req (coordinator brief, 2026-10-10): "Operator decision ON-12 (ledger Q-349, relayed by praetor-07): switch vmafx to the shared FFmpeg fix series now, during the freeze. Normal PR, merge on green."
- Q-340: the FFmpeg fix patches moved to VMAFx/ffmpeg-patches.
- [ADR-1240](1240-ffmpeg-release-patch-lifecycle.md) — the release-tag lifecycle this extends.
- [FFmpeg patch automation](../development/ffmpeg-patch-automation.md#shared-ffmpeg-fix-series)
