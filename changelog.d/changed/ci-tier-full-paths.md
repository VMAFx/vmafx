- **CI tier escalates own pull requests touching platform process-launch paths
  to `full` ([ADR-3536](docs/adr/3536-ci-tier-full-paths.md)).**
  `.github/ci-tier.json` adds `full_paths` (`fnmatch.fnmatchcase` globs) for
  platform-guarded process-launch modules and their tests so unlabelled own
  pull requests touching them run the full cross-platform suite, fail closed to
  `full` when changed paths are unavailable or any file or label API entry is
  malformed, and check `full_paths` against `git ls-files` via
  `scripts/ci/ci_tier.py --check-full-paths` with repository-locating `GIT_*`
  variables dropped and only an absolute `GIT_INDEX_FILE` kept for this
  checkout.
