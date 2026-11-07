## FFmpeg shared fix series v0.6.0 (2026-11-06)

- `build-config.env`: `FFMPEG_FIX_SERIES_TAG` and `FFMPEG_FIX_SERIES_SHA256` move to `VMAFx/ffmpeg-patches` `v0.6.0` (commit `8f88544085`). The release's `base.env` still names FFmpeg `n9.0.2` / `946fcce0`, so `FFMPEG_TAG` and `FFMPEG_COMMIT` stay. On a conflict take the newer series pin of the two and run `python3 scripts/ci/ffmpeg_patch_stack.py --refresh`, then `--check`.
- The series adds `0040`-`0042` (`vf_libvmaf.c`: more planar formats, all planes of CUDA frames, CUDA context pop on a failed plane copy) and replaces `0037` (`aom_film_grain`). The six VMAFx patches replay on it unchanged; only `ffmpeg-patches/README.md` records the new series and source revision.
