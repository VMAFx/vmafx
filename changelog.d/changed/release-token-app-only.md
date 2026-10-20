- **Release workflows use only the release-bot App token** ([ADR-3356](docs/adr/3356-release-token-app-only.md)):
  `release-please.yml` and the macOS and Windows tester-bundle workflows mint
  the App installation token unconditionally and no longer fall back to the
  `RELEASE_BOT_TOKEN` personal access token. Without the App secrets a run now
  fails at the mint step instead of finishing green with a warning.
