---
paths:
  - renovate.json
  - scripts/ci/tests/test_renovate_file_patterns.py
  - docs/development/dependency-bot.md
invariant: Mend Renovate GitHub App (ADR-0387) reads in-tree renovate.json; dependabot.yml stays disabled.
---
# Renovate dependency-update bot (ADR-0363)

## Dependency-update bot: Renovate, not Dependabot (ADR-0363)

Fork uses **Mend Renovate** as a GitHub App (ADR-0387), not self-hosted. The
App reads the in-tree [`renovate.json`](../../renovate.json) and opens grouped
dependency-update PRs on the configured weekday schedule.
`.github/dependabot.yml` has been removed, content archived as
`.github/dependabot.yml.disabled`.

On upstream sync:

- If Netflix adds `dependabot.yml`, **never** restore it — merge content
  into `dependabot.yml.disabled` for reference only. Fork's dependency-update
  bot is Renovate; running both simultaneously causes duplicate PRs.
- `renovate.json` is fork-local; Netflix upstream will never ship it. It is
  safe from upstream conflicts.
- Operator playbook is
  [dependency-bot.md](../../docs/development/dependency-bot.md).

## Renovate (ADR-0363) supersedes Dependabot

Note: pin updates to `codeql-action/upload-sarif` now arrive via Renovate
(grouped with other GitHub Actions minor+patch bumps), not Dependabot.

## CI Python interpreter group (ADR-3312)

`setup-python` `python-version:` pins: not `github-actions` manager's;
package rule disables dep `python` there. Owner: `custom.regex` manager,
together with `PYTHON_CI_VERSION`, group "CI Python interpreter", `<3.15.0` until
1.0.0. Fixture: `PythonCiVersionGroup` in
`scripts/ci/tests/test_renovate_file_patterns.py`.
