<!-- markdownlint-disable MD060 -->
# ADR-0317: Path-filter Docker + FFmpeg-integration on doc/Python-only PRs

- **Status**: Accepted
- **Date**: 2026-05-06
- **Deciders**: Lusoris, Claude
- **Tags**: `ci`, `build`

## Context

The recent doc / Python-only merge train (PRs #409, #411, #412) saw two
non-required CI jobs go red on every push:

1. **`Docker Image Build`** (`.github/workflows/docker-image.yml`) — failed
   inside the image's `apt-get install` step with exit code 100. Logs from
   run `25118138131` (PR #411 head `70b036e5`) show the failure is a
   transient apt-mirror flake on the Ubuntu base image; the diff under test
   contains zero Dockerfile, libvmaf, or build-system changes.
2. **`FFmpeg — SYCL (Build Only)`** in
   `.github/workflows/ffmpeg-integration.yml` — failed at
   `make install` of FFmpeg with `vf_libvmaf_tune.c:178:
   ‘AVFilterLink’ has no member named ‘frame_rate’` (run `25416488158` on
   master `11d97855`). This is a real fork-local bug introduced by
   patch `0008-add-libvmaf_tune-filter.patch` (ADR-0312, PR #409): `frame_rate`
   moved off `AVFilterLink` in FFmpeg n7+ and the patch did not migrate to
   `ff_filter_link()` the way patches 0005 / 0006 already do. The full-
   `make install` lane catches this; the narrow `vf_libvmaf.o`-only Vulkan
   lane does not, which is why only SYCL goes red.

Both jobs are **non-required** — the `Required Checks Aggregator` (ADR-0313)
gates merge on a fixed list of 23 build/lint checks that excludes Docker,
FFmpeg-SYCL, and FFmpeg-Vulkan. So merge was unblocked, but every push to a
doc-only PR cost ~10–15 min of runner time and produced two red checks that
reviewers had to mentally filter out. Path-filtering the workflows so they
fire only on diffs that actually exercise their inputs eliminates the noise
without weakening the merge gate.

The underlying patch-0008 bug is real and remains as a follow-up — it will
surface as soon as a libvmaf/ or ffmpeg-patches/ change runs the SYCL lane,
which is exactly when it should be caught and fixed. This ADR is about the
flake on irrelevant diffs, not the upstream-API drift.

## Decision

We will add `paths:` filters to both `docker-image.yml` and
`ffmpeg-integration.yml` so they trigger only when their actual inputs
change. For Docker: `Dockerfile`, `.dockerignore`, `libvmaf/**`,
`meson.build`, `meson_options.txt`, the workflow file itself. For FFmpeg
integration: `libvmaf/**`, `ffmpeg-patches/**`, `meson.build`,
`meson_options.txt`, the workflow file itself. Neither workflow is a
required check (per ADR-0313's aggregator allow-list), so the merge gate is
unaffected.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Path-filter both workflows (chosen) | Eliminates the flake on the exact PR class where it's noise (doc/Python-only); preserves coverage on diffs that actually matter; matches the existing `docs.yml` precedent on this fork; small diff. | Path-filter rules can drift from build inputs over time and silently skip a workflow that should have fired; needs maintenance whenever the build picks up a new top-level input. | Chosen — the maintenance cost is bounded (one-line additions) and the precedent already exists for `docs.yml`. |
| Add `continue-on-error: true` to FFmpeg-SYCL (Docker already has it) | One-line change; keeps coverage on every push. | Doesn't fix the root cause of runner-time waste; jobs still consume 10–15 min per push and still display red, just without blocking merge — exactly the state the merge train was already in and that motivated this PR. | Rejected — the user's brief explicitly flagged the runner-time burn, not just the merge-gate impact. |
| Quarantine to nightly (move both to a scheduled `cron:` workflow) | Zero per-push cost; still catches regressions within 24 h. | Loses the per-PR signal — a Dockerfile or ffmpeg-patches change wouldn't be caught until the nightly run, after merge; the bug would land on master with no pre-merge gate. | Rejected — per-PR coverage on the inputs that *do* exercise these jobs is exactly what we want; we just don't want the firing on inputs that don't. |
| Fix patch 0008 in this PR (use `ff_filter_link()` like 0005/0006) | Closes the SYCL-lane real bug at the same time. | Out of scope for the brief ("fix two CI flakes"); changing an `ffmpeg-patches/` file is a non-trivial integration concern that wants its own PR + series replay verification per CLAUDE rule 14; conflates two unrelated reviews. | Rejected — tracked as a follow-up; the path-filter also doesn't hide it (any libvmaf/ or ffmpeg-patches/ PR will still run SYCL and surface the bug). |

## Consequences

- **Positive**: Doc-only / Python-only / vmaf-tune-only PRs no longer
  trigger the two flaky jobs; the merge train gets ~30 min of runner time
  back per PR; review surface is cleaner (no red non-required checks).
- **Negative**: A regression that only manifests when one of these jobs
  runs on a diff outside the path filter (e.g. a Python change that
  somehow alters Docker behaviour) wouldn't be caught pre-merge. The risk
  is theoretical — both workflows build C/C++ artefacts that have no
  Python-side coupling.
- **Neutral / follow-ups**:
  - Patch 0008 (`vf_libvmaf_tune.c`) needs its own PR migrating
    `outlink->frame_rate = mainlink->frame_rate;` to use
    `ff_filter_link(outlink)->frame_rate` /
    `ff_filter_link(mainlink)->frame_rate`, matching the pattern in
    patches 0005 / 0006. The path-filter does not
    suppress this bug; it surfaces on the next libvmaf/ or
    ffmpeg-patches/ touching PR.
  - If the path filter list ever drifts from the actual build inputs,
    `workflow_dispatch:` is preserved on both workflows so a maintainer
    can manually trigger a run.

## References

- ADR-0313 — Required Checks Aggregator (defines which checks block merge;
  Docker + FFmpeg-SYCL are not in the list).
- ADR-0312 — vmaf-tune ffmpeg-patches integration (introduced patch 0008
  whose `AVFilterLink::frame_rate` reference is the SYCL lane's deterministic
  failure).
- PRs #409, #411, #412 — the doc/Python-only merge train where the flake
  pattern was observed.
- `.github/workflows/docs.yml` — existing path-filter precedent on this fork.
- Source: `req` (direct user instruction to fix two persistent CI flakes
  tripping on doc/Python-only merge train, paraphrased: "they're burning
  10–15 minutes of runner time per push on PRs that don't even touch C/C++
  or Dockerfile").
