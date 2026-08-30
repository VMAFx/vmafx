- **`Docker Image Build` and `FFmpeg — SYCL (Build Only)` flakes on
  doc/Python-only PRs.** Both workflows triggered on every push to PRs
  that touched zero Dockerfile, libvmaf, or ffmpeg-patches inputs (the
  doc/Python-only merge train: PRs #409, #411, #412), burning ~10–15 min
  of runner time per push and leaving two red checks on each PR.
  `docker-image.yml` failed inside the image's `apt-get install` step
  with exit code 100 — a transient apt-mirror flake on the Ubuntu base.
  `ffmpeg-integration.yml`'s SYCL lane (the only matrix entry that does
  a full `make install` of FFmpeg) failed deterministically at
  `vf_libvmaf_tune.c:178: 'AVFilterLink' has no member named
  'frame_rate'` — a real but pre-existing fork-local bug from patch
  `0008-add-libvmaf_tune-filter.patch` (ADR-0312, PR #409): `frame_rate`
  moved off `AVFilterLink` in FFmpeg n7+ and the patch did not migrate
  to `ff_filter_link()` the way patches 0005 / 0006 already do. Fix
  adds `paths:` filters to both workflows so they fire only when their
  actual inputs change (`Dockerfile` + `libvmaf/**` + build-system for
  Docker; `libvmaf/**` + `ffmpeg-patches/**` + build-system for FFmpeg
  integration). Neither workflow is in the `Required Checks Aggregator`
  (ADR-0313) allow-list, so the merge gate is unaffected;
  `workflow_dispatch:` preserved on both for manual triggers. The
  underlying patch-0008 bug is tracked as a follow-up — the path-filter
  does not suppress it; it surfaces on the next libvmaf/ or
  ffmpeg-patches/ touching PR. See
  [ADR-0317](../../docs/adr/0317-ci-doc-only-pr-flake-fix.md).
