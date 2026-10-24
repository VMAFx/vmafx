- CI: artifact steps no longer fail on the Gitea mirror with
  `GHESNotSupportedError`. Each `upload-artifact` / `download-artifact` step
  runs off Gitea only and has a Gitea twin on v3, the artifact protocol Gitea
  serves; a download that needs inputs v3 lacks prints a warning naming them
  instead. `scripts/ci/check_gitea_artifact_steps.py` (in `make lint-actions`
  and pre-commit) refuses an unpaired step
  ([ci-runners](docs/development/ci-runners.md#gitea-mirror-runner-artifact-steps),
  ADR-3368).
