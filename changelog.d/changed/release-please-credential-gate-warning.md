- `release-please.yml` no longer fails every push to `master` while the release-bot
  App credentials are missing: the credential check emits a warning, skips every
  write step, and the run ends idle-green; a manual `workflow_dispatch` still fails.
  `scripts/release/check-release-bot-secrets.sh` (part of `/prep-release`) asserts
  the two secret names exist before a release is attempted (ADR-1171).
