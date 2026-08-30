- **CI**: new `Required Checks Aggregator` workflow
  ([`.github/workflows/required-aggregator.yml`](.github/workflows/required-aggregator.yml),
  [ADR-0313](docs/adr/0313-ci-required-checks-aggregator.md)) that runs on
  every non-draft PR and verifies the 23 named required checks each
  reported `success`, `skipped`, or `neutral` (or didn't appear at all,
  which is the documented "path-filter rejection" semantics). Replaces
  the 23-check required-list under branch protection with this single
  aggregator. Unblocks doc-only / Python-only PRs (which previously hit a
  structural deadlock because the C-build matrix path-filter-skipped on
  their diffs but branch protection still required those check names to
  report). The 23 individual workflows continue to run unchanged — only
  the protection-layer required-list flips.
