---
paths:
  - scripts/ci/tidy-drift-issue.sh
  - scripts/ci/tests/test_tidy_drift_issue.py
  - scripts/ci/tests/test_tidy_lanes_hosted_contract.py
invariant: Every tidy lane is a required, path-routed check that says when it did not measure; nightly sweep keeps one drift issue.
area: tidy
---
<!-- markdownlint-disable MD013 MD060 -->
# Hosted tidy lanes (ADR-2796, Q-315)

Every lane is a required, path-routed check: `Tidy Ratchet` (cpu), `Tidy Lane
(<lane>)` for cuda/hip/sycl/arm64/clang (matrix job `clang-tidy-lanes` in
`lint-and-format.yml`, dev container pinned by digest in
`.github/actions/tidy-lane/action.yml`), `Tidy Metal` (`tidy-metal.yml`).
Selectors `tidy_<lane>` in `.github/ci-impact.json` are `own_paths_only`; a lane
file only that lane measures must match its selector
(`test_ci_impact.py::TidyLaneRouting`). A skipped leg prints "NOT measured"; the
nightly sweep (`nightly.yml`) runs all lanes and opens/updates one issue via
`tidy-drift-issue.sh`. Contract: `tests/test_tidy_lanes_hosted_contract.py`.
Bump the image digest with the baselines after a `dev/Containerfile` change.
