## FFmpeg shared fix series v0.2.0 (2026-10-15)

- `build-config.env`: `FFMPEG_FIX_SERIES_TAG` and `FFMPEG_FIX_SERIES_SHA256`
  move to `VMAFx/ffmpeg-patches` `v0.2.0` (commit `d958384a`). The release's
  `base.env` still names FFmpeg `n9.0.2` / `946fcce0`, so `FFMPEG_TAG` and
  `FFMPEG_COMMIT` stay. On a conflict take the newer series pin of the two
  and run `python3 scripts/ci/ffmpeg_patch_stack.py --refresh`, then `--check`.
- `docs/development/ffmpeg-patch-automation.md`: patch table lists 0005-0008.
  Fork-only, nothing upstream to merge.
