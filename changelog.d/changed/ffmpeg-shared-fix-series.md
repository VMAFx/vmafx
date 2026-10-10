- **FFmpeg builds take FFmpeg's own fixes from the shared fix series.** Every VMAFx FFmpeg build
  (images, CI, `ffmpeg-patches/test/build-and-run.sh`) now applies the series of
  [VMAFx/ffmpeg-patches](https://github.com/VMAFx/ffmpeg-patches) `v0.1.0-rc.1` before
  `ffmpeg-patches/`, pinned by tag and sha256 in `build-config.env` and verified by
  `scripts/ci/ffmpeg-shared-series.sh` (ADR-3143). Patch 0019 leaves this repository's series: it is
  the shared series' diagnostics patch. Three shared patches change FFmpeg's behaviour: a Vulkan frame
  on a single-queue-family device is no longer released to `VK_QUEUE_FAMILY_IGNORED`; `hevc_nvenc`
  with `udu_sei=1` drops a user data SEI over NVENC's 1024-byte non-VCL limit with a warning instead
  of failing the encode; `h264_nvenc` and `hevc_nvenc` drop a user data SEI that NVENC would write
  truncated. To apply the series by hand: `scripts/ci/ffmpeg-shared-series.sh apply TREE`.
