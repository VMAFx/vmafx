- Dependency maintenance: Renovate now updates Node.js across `build-config.env`
  (`NODE_VERSION`), `.pre-commit-config.yaml` (`default_language_version.node`),
  and `.github/workflows/` (`node-version:`) in one pull request. Previously,
  the built-in GitHub Actions manager updated only workflow files, leaving the
  other pins behind and failing the single-source version gate. A custom regex
  manager now groups them as `CI Node.js` (ADR-3312 pattern).
