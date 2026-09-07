- `release-please.yml` now accepts a `RELEASE_BOT_TOKEN` PAT as an alternative release-bot
  identity when the GitHub App secrets are absent. PRs opened by `GITHUB_TOKEN` never
  receive check runs, so the release PR could never satisfy branch protection — and the
  App, which was the only accepted identity, has no API path to create. The pipeline is no
  longer blocked on a browser-only step. The App remains preferred and is still used when
  its secrets exist; the resolved token is masked either way. See
  [docs/development/release.md](docs/development/release.md) § Release-bot identity.
