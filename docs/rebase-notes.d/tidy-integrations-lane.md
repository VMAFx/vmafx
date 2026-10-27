## Tidy lane `integrations` for the GStreamer element and FFmpeg sources (2026-10-27)

- `scripts/ci/tidy-integrations-build.sh`, `scripts/ci/gen-integrations-compile-commands.py`
  (new): build the element and the patched FFmpeg tree next to the core
  build and add their compile lines to the lane's database. Fork-only.
- `scripts/ci/ffmpeg_patch_stack.py`: `--keep-tree DIR` keeps the replayed tree.
- A new `.c` file under `ffmpeg-patches/src/` needs a `FFMPEG_UNITS` row in the
  generator and an object in the build script, or the lane fails.
- No upstream file is touched; nothing to merge from Netflix/vmaf.
