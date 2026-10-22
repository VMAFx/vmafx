## Release workflows take only the App token (2026-10-19)

- `.github/workflows/release-please.yml`, `macos-tester-bundle.yml`,
  `windows-tester-bundle.yml`: the mint step (`id: app-token`) has no `if:`,
  and every release write reads `steps.app-token.outputs.token` directly
  ([ADR-3356](../adr/3356-release-token-app-only.md)). Fork-only workflows,
  nothing upstream to merge. **On rebase**: do not bring back the `creds`
  step, the token-resolving step or a `RELEASE_BOT_TOKEN` fallback; praetor's
  release-token audit fails on any of them.
