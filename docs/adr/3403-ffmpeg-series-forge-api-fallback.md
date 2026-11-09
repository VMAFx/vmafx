<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3403: Shared FFmpeg fix series forge API download fallback

- **Status**: Accepted
- **Date**: 2026-11-09
- **Deciders**: Lusoris (controller ruling 06:31)
- **Tags**: build, ci, docker, ffmpeg, supply-chain

## Context

`scripts/ci/ffmpeg-shared-series.sh` downloads the shared FFmpeg fix series
([VMAFx/ffmpeg-patches](https://github.com/VMAFx/ffmpeg-patches)) release tarball
and `SHA256SUMS` from GitHub releases using `curl` or `wget`. Every VMAFx FFmpeg
build applies this series before `ffmpeg-patches/`
([ADR-3143](3143-ffmpeg-shared-fix-series.md)). Furthermore, the pre-commit hook
`ffmpeg-patches-apply-check` runs `ffmpeg-shared-series.sh` for every commit
touching `ffmpeg-patches/`, `scripts/ci/`, `build-config.env`, `core/include/`, or
meson build files.

On 2026-11-09, direct anonymous HTTP requests to
`https://github.com/VMAFx/ffmpeg-patches/releases/download/...` encountered
repeated HTTP 504 Gateway Timeouts due to upstream CDN/forge timeouts. Because
`download()` relied exclusively on `curl` or `wget` against the public release URL,
every commit touching in-scope files was blocked. In contrast, the authenticated
forge API (`gh api repos/...` and `gh release download`) remained operational.

## Decision

`scripts/ci/ffmpeg-shared-series.sh` introduces `release_fallback()` to handle
download failures on `https://github.com/...` release asset URLs.

1. When `curl` or `wget` fails for an `https://` release asset URL matching
   `^https://github\.com/([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+)/releases/download/([A-Za-z0-9_.+-]+)/([A-Za-z0-9_.+-]+)$`,
   the script falls back to `gh release download "$tag" --repo "$repo" --pattern "$asset" --output -`.
   URLs on other hosts or lookalike domains do not trigger the fallback and fail with
   the original error message.
2. The fallback stream is piped to `head -c $((DOWNLOAD_MAX_BYTES + 1))` into the destination
   file, with `DOWNLOAD_MAX_BYTES` bounded at 64 MiB (67,108,864 bytes). If the asset exceeds
   this bound or if either pipeline command fails (verified via `PIPESTATUS`), the destination
   is deleted and the script aborts.
3. Execution of `gh` is bounded by a portable timeout helper `run_with_deadline` using
   `DOWNLOAD_SECONDS` (default: 120, overridable by `FFMPEG_FIX_SERIES_DOWNLOAD_SECONDS`
   matching `^[1-9][0-9]{0,5}$`).
4. Informative messages explicitly record when the fallback path runs. Under GitHub Actions
   (`GITHUB_ACTIONS=true`), an Actions warning is emitted to stdout.
5. All post-download verification gates remain unchanged: sha256 checksum against
   `FFMPEG_FIX_SERIES_SHA256`, verification against `SHA256SUMS`, and cosign signature
   verification where enabled.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Wait for forge recovery | Zero script modifications | Blocks every commit touching patched areas during anonymous forge outages | Unacceptable developer and CI disruption |
| Hand-seeded cache | Skips network fetch | Bypasses the download; hard to distribute hermetically across environments | Defeats clean container and CI verification; refused |
| Third-party mirror | Bypasses github.com outage | Introduces an external trust root and requires mirror sync automation | Adds unneeded supply-chain complexity and maintenance burden |
| Forge API via `gh release download` | Same release assets; preserves existing sha256, checksum and signature checks | Requires `gh` CLI on environments needing fallback | Chosen: transparent, authenticated access to identical signed release artifacts |

## Consequences

- **Positive**: Commits and CI runs touching patch automation or core headers are resilient
  against anonymous forge CDN outages. Integrity guarantees are preserved without loosening
  any verification rules.
- **Negative**: Environments requiring fallback must have `gh` available on `PATH`. If `gh`
  is missing, the script prints an informative message and exits with status 1.
- **Neutral / follow-ups**: Containers without `gh` continue to rely on `curl` or `wget` and
  will fail cleanly if forge downloads fail. `FFMPEG_FIX_SERIES_DOWNLOAD_SECONDS` allows
  fine-grained timeout control in automated tests and resource-constrained environments.

## Supply-chain impact

- **Build-time fetches**: Retrieves the identical pinned release artifacts (`ffmpeg-patches-<tag>.tar.gz`,
  `SHA256SUMS`) via `gh release download` through the GitHub Releases API when public HTTP CDN
  fetches fail.
- **Dependencies**: Uses existing `gh` CLI when installed. No new external libraries or remote mirrors.

## References

- Controller ruling 06:31: reuse the API-path fallback shape for release downloads.
- [ADR-3143](3143-ffmpeg-shared-fix-series.md) — VMAFx consumes the shared FFmpeg fix series.
- [FFmpeg patch automation](../development/ffmpeg-patch-automation.md#shared-ffmpeg-fix-series).
