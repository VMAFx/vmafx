- The deliverables gate (`scripts/ci/deliverables-check.sh`) and the pull
  request template recognise `small PR (ADR-2461)`: for a pull request of at
  most 100 changed lines in one top-level directory that touches no source of
  `core/`, public header, CLI, build option, golden test, FFmpeg patch or ADR,
  the research digest, decision matrix, `AGENTS.md` note and rebase note are
  waived. The marker is refused, with the reasons, when the diff does not
  qualify. See `docs/development/pr-body-sentinel-guide.md`.
