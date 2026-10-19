- **Mini retrain and a resumable stage runner for the retrain tooling** (ADR-1898, issue #1246).
  `make mini-retrain` runs extraction, feature checks, combination, training and export of
  `vmaf_tiny_v2` to `v4` and `fr_regressor_v1`, validation, registry validation and a PLCC / SROCC / RMSE
  gate on a generated 144-row corpus in about 40 seconds. Every stage writes a manifest with seed,
  digests, library versions, lock digest, container id and resource use; a killed run resumes from the
  manifests; a missing or corrupt input stops the run with the stage name before anything runs. The
  Tiny AI job runs it for changes under `ai/`, and a nightly workflow runs it too. See the runbook section 13.
