## FFmpeg shared fix series v0.5.0 (2026-10-27)

- `build-config.env`: `FFMPEG_FIX_SERIES_TAG` and `FFMPEG_FIX_SERIES_SHA256` move to `VMAFx/ffmpeg-patches` `v0.5.0` (commit `7641804bdd`). The release's `base.env` still names FFmpeg `n9.0.2` / `946fcce0`, so `FFMPEG_TAG` and `FFMPEG_COMMIT` stay. On a conflict take the newer series pin of the two and run `python3 scripts/ci/ffmpeg_patch_stack.py --refresh`, then `--check`.
- `scripts/ci/ffmpeg-shared-series.sh`: `check_listed_sum` compares the tarball's sha256 with the release's `SHA256SUMS` in every verify mode and prints both; keep it with `test_tarball_not_listed_in_sha256sums_is_refused`.
- `.github/workflows/ffmpeg-integration.yml`: `ffmpeg-work` and `ffmpeg-sycl-work` name `ubuntu-26.04` again; `.config/lint-exceptions.d/hosted-runner-label.toml` is gone. A rebase must not bring either fallback back.
