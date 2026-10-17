- **CI: three jobs that fail on the `ubuntu-26.04` image run on `ubuntu-24.04` again until
  2026-10-25 (ADR-3251).** Since every hosted Linux job names `ubuntu-26.04`, the dev
  container build ran out of disk space and the FFmpeg gcc and SYCL builds stopped at the
  zero-warning gate on warnings the newer gcc prints in FFmpeg's own sources.
  `dev-container-build-work`, `ffmpeg-work` (its `FFmpeg Ubuntu gcc` row) and
  `ffmpeg-sycl-work` carry a dated fallback in
  `.config/lint-exceptions.d/hosted-runner-label.toml`; the check fails on an entry after its
  end date. The declared exception list now takes one `hosted-runner-label` entry per job, so
  two jobs of one workflow file can each hold one; every other rule keeps one entry per file.
  No score, symbol, option or published artefact changes.
