## FFmpeg builds apply the shared fix series before `ffmpeg-patches/` (2026-10-10)

- `build-config.env` pins `FFMPEG_FIX_SERIES_REPO`, `_TAG` and `_SHA256`
  (ADR-3143). `scripts/ci/ffmpeg-shared-series.sh` is the one place that
  downloads, verifies and applies the series; `ffmpeg_patch_stack.py`,
  `ffmpeg-patches/test/build-and-run.sh`, `ffmpeg-integration.yml`,
  `Dockerfile`, `Dockerfile.ffmpeg`, `dev/Containerfile` and
  `docker/Dockerfile.node` call it before they apply `series.txt`.
- `ffmpeg-patches/0019-*` is gone and its number is retired: the diagnostics
  hardening is patch 0002 of the shared series. A sync or a new patch must not
  bring a fix to FFmpeg itself back into `ffmpeg-patches/`; it belongs in
  VMAFx/ffmpeg-patches.
- A new FFmpeg tag needs a series release whose `base.env` names it: move the
  three pins together with `FFMPEG_TAG` / `FFMPEG_COMMIT`, then run
  `python3 scripts/ci/ffmpeg_patch_stack.py --refresh`.
- `SharedFixSeriesContract` (`scripts/ci/test_ffmpeg_patch_workflow_contract.py`)
  fails when a build applies `ffmpeg-patches/` without the script before it.
