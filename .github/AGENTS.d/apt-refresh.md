---
paths:
  - .github/workflows/*.yml
  - .github/actions/*/action.yml
invariant: Hosted-runner apt refresh runs only through scripts/ci/apt-install-cached.sh refresh|install.
---
# Hosted-runner APT index refresh

Workflows and composite actions refresh APT package lists only via
`scripts/ci/apt-install-cached.sh` (`install` for cached package sets, `refresh`
before custom `apt-get install` steps after `actions/checkout`):

```yaml
bash scripts/ci/apt-install-cached.sh refresh
```

Direct `sudo apt-get ... update` in `.github/workflows/*.yml` or
`.github/actions/*/action.yml` fails `AptUpdateFailClosedContract`. Container
commands without `sudo` (such as `docker run ... apt-get --error-on=any -o
Acquire::Retries=3 update`) remain governed by general refresh rules.

Full checker rules:
[apt-update-fail-closed](../../scripts/ci/AGENTS.d/apt-update-fail-closed.md)
and [ADR-3537](../../docs/adr/3537-apt-update-fail-closed.md).
