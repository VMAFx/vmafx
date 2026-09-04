- **ci**: shorten CI job and matrix display names across all workflows
  to $\le 30$ characters, stripping redundant parentheticals, verbose
  suffixes, and inline ADR/policy citations while preserving essential
  matrix qualifiers. Added `scripts/ci/check-aggregator-names.sh` (wired
  into `make lint-sh` and pre-commit) to gate 1:1 parity between
  `required-aggregator.yml` and `# required-aggregator` markers in workflow
  definitions. Added `docs/development/ci-job-names.md` documenting the
  full mapping table and conventions.
