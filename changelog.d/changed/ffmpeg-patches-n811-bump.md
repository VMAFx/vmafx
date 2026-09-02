- `ffmpeg-patches/` — base FFmpeg version bumped from **n8.1** to
  **n8.1.1** (point release, bug-fix-only on top of n8.1). All 9
  patches in the stack
  (`0001-libvmaf-add-tiny-model-option` through
  `0009-pass-autotune-cli-glue`) apply cleanly cumulatively against
  pristine `n8.1.1` via `git am --3way`; no patch regeneration
  needed. Updated string references in `series.txt`, `README.md`,
  `test/build-and-run.sh` (`FFMPEG_SHA` default), the local
  pre-push gate at `scripts/ci/ffmpeg-patches-check.sh`, and the
  enforcement-table row in
  `docs/development/automated-rule-enforcement.md`. The
  `FFMPEG_BRANCH` env default in the CI helper stays at
  `release/8.1` because that branch tracks the point-release
  series; n8.1.1 is its current HEAD. See
  [docs/rebase-notes.md §0320](../../docs/rebase-notes.md) for the
  verification command.
