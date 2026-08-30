<!-- markdownlint-disable MD060 -->
# Dependency-update bot — operator playbook

The fork uses **Mend Renovate** as a [GitHub App][app], not self-hosted.
The App reads the in-tree [`renovate.json`](../../renovate.json) and opens
grouped dependency-update PRs continuously (no schedule throttling).

[app]: https://github.com/apps/renovate

## Quick start

1. Visit <https://github.com/apps/renovate> and install the App on
   `VMAFx/vmafx`.
2. The App posts a Dependency Dashboard issue (currently
   [#749](https://github.com/VMAFx/vmafx/issues/749)) listing pending /
   awaiting / errored updates.
3. Tick a checkbox in the dashboard issue to force creation of any
   awaiting update; the App reacts within a minute or two.

## Configuration

All configuration lives in [`renovate.json`](../../renovate.json). The
App reads it on every webhook. Top-level knobs:

| Setting | Value |
|---------|-------|
| `schedule` | `at any time` |
| `prHourlyLimit` | `0` (unlimited) |
| `prConcurrentLimit` | `12` |
| `prCreation` | `immediate` |
| `minimumReleaseAge` | `3 days` |

## Disable / rollback to Dependabot

1. Uninstall the App at `https://github.com/settings/installations`.
2. Rename `.github/dependabot.yml.disabled` → `.github/dependabot.yml`.

## Migration from self-hosted (2026-05-10)

Removed `.github/workflows/renovate.yml`. The App's webhook-driven model
replaces the cron-driven workflow. The `RENOVATE_TOKEN` secret is no
longer needed and can be deleted from repo secrets after install.

See [ADR-0387](../adr/0387-renovate-github-app-migration.md) for the
decision record (supersedes the self-hosted half of ADR-0363).
