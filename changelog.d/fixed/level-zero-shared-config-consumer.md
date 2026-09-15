- Make development-container Level Zero downloads consume `LEVEL_ZERO_VERSION`
  from `build-config.env`, with tests for real command consumption and drift.
  Renovate now updates the shared loader setting; remove its obsolete ROCm
  literal-version manager because the central image pins already own ROCm.
