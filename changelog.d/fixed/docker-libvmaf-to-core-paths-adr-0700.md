- **Docker Image Build silent-red since 2026-05-28 (ADR-0700 fallout).**
  The `libvmaf/` → `core/` source-tree rename did not migrate two
  build-system constants that the root `Dockerfile` consumes:
  `Makefile:24` (`LIBVMAF_DIR := libvmaf`) and `Dockerfile:88`
  (`ENV PATH=/vmaf:/vmaf/libvmaf/build/tools:...`). The
  `docker build` step then failed with `Neither source directory
  'libvmaf/build' nor build directory 'libvmaf' exist`, but
  `.github/workflows/docker-image.yml:51` carried
  `continue-on-error: true` (per T7-CI-DEDUP / Research-0034 demotion),
  so the job-level red was masked at the workflow level for ~48 hours
  across every master push. PRs #293 and #294 inherited the silent
  red but were not the introducer. Fix: rename both paths to `core/`
  and flip `continue-on-error: false` so a future rename-class
  regression surfaces immediately. T7-DOCKER-SMOKE (real
  `docker run vmaf vmaf --version`) tracked as a follow-up.
