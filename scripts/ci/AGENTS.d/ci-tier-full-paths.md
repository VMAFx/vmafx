---
paths:
  - scripts/ci/ci_tier.py
  - scripts/ci/tests/test_ci_tier.py
  - .github/ci-tier.json
  - .github/workflows/ci-tier.yml
  - .pre-commit-config.yaml
invariant: Path match, config change, unknown paths -> full; no glob drift.
area: gates
---
# CI tier full paths (ADR-3536, amends ADR-2169)

- Owner: `ci_tier.py`; definition: `.github/ci-tier.json`; workflow:
  `.github/workflows/ci-tier.yml`. Non-exempt own PR: listed path -> `full`.
  Unlisted path alone -> `light`. Matching: `fnmatch.fnmatchcase`.
- `TIER_CONFIG_PATH` = shared relative config filename. Config change ->
  `full` before glob matching, including deleted listed file + removed glob,
  or empty `full_paths`. Preserve `MAX_CHANGED_PATHS`, `MAX_FULL_PATHS`.
- Unknown changed paths -> `full`, reason `changed paths unavailable`.
  Release exemption: unknown paths -> false; never replace unknown with empty
  sequence. Fetched release-only paths -> `release-light`; cut label -> full.
  File fetch cache shared between release exemption and path matching.
- Release exemption + config change -> `full` before cut-label check; shared
  helper `_tier_config_decision`.
- Every glob matches tracked file. Drift hook `check-ci-tier-full-paths`:
  `.pre-commit-config.yaml`; always run at pre-commit and pre-push.
  `ci_tier.py --check-full-paths`: unmatched glob -> non-zero + named pattern.
- Proof: `tests/test_ci_tier.py`, classes `Decide`, `RunFullPaths`,
  `FullPathsDrift`. Cases: deleted listed path + removed glob; config alone;
  release config change; unlisted path; glob boundary; unknown release diff;
  release-only diff; cut label; tracked rename + restoration; path-count
  boundaries.
- Audit: [Research-3536](../../../docs/research/3536-ci-tier-full-paths.md).
  Decision: [ADR-3536](../../../docs/adr/3536-ci-tier-full-paths.md).
