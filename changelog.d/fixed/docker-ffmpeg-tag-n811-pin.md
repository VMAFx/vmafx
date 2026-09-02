- **Docker FFMPEG_TAG pin drift fixed** — `Dockerfile` and `Dockerfile.ffmpeg` both
  declared `ARG FFMPEG_TAG=n8.1` (the bare minor-version tag) rather than
  `ARG FFMPEG_TAG=n8.1.1` (the patch-level tag our 16-patch stack targets).
  Building against bare `n8.1` risked patch-context cascade failures because `n8.1`
  and `n8.1.1` differ by at least one commit (`Bump micro for 8.1.1`).  Both files
  are now pinned to `n8.1.1`, matching `ffmpeg-patches/series.txt`.
- **Patch 0016 context fixed** —
  `0016-libvmaf-wire-score-fmt-on-all-vmaf-filters.patch` had a stale hunk-#1
  context line referencing `gpumask bit 1 disables` (the wording before
  patch 0014 rewrote it to `gpumask bit 0 (value 1)`).  The context is now
  updated; all 16 patches replay cleanly against a fresh `n8.1.1` checkout via
  `git am --3way`.
