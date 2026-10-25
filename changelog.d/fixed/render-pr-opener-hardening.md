- CI: the scheduled render pull request opener (`scripts/ci/open-render-pr.sh`) acts only on a
  same-repository pull request the release-bot App opened, re-arms auto-merge when the branch is
  already rendered at the master tip, commits a render that adds only new files, and refuses to
  start without `RUNNER_TEMP` (ADR-3373).
