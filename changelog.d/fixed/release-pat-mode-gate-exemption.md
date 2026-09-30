- `scripts/ci/release-pr-exempt.sh` and pre-push hooks now exempt PAT-mode
  `release-please` pull requests (`RELEASE_BOT_TOKEN`, author `lusoris`, `type: User`)
  from authoring-discipline CI gates (ADR-1388, closes #1608). The exemption is
  fail-closed: it requires the designated PAT author and verifies that 100% of the
  files in the PR diff belong strictly to the approved release file set
  (`.release-please-manifest.json`, `release-please-config.json`, `CHANGELOG.md`,
  `changelog.d/*`, `docs/changelog-archive/*`, and coordinated version markers).
