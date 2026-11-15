---
paths:
  - scripts/release/check-release-bot-secrets.sh
  - scripts/release/tests/test-check-release-bot-secrets.sh
  - .github/workflows/release-please.yml
invariant: Require RELEASE_BOT_APP_ID and RELEASE_BOT_PRIVATE_KEY; never add GITHUB_TOKEN fallback.
---
<!-- markdownlint-disable MD013 MD060 -->
# Release-bot secret preflight (`check-release-bot-secrets.sh`, ADR-1171)

## check-release-bot-secrets.sh (ADR-1171)

Preflight for release-bot identity: `gh secret list` must show both
`RELEASE_BOT_APP_ID` and `RELEASE_BOT_PRIVATE_KEY`; exit 1 when name
missing, 2 when `gh` cannot list. Invariant: `release-please.yml`
stays idle-green on `push` without secrets (warning, every write step
skipped). This script = only loud local signal that release cannot
be cut — `/prep-release` step 1 runs it, treats non-zero as NO-GO.
Never add `GITHUB_TOKEN` fallback to workflow to "fix" idle run. Test
coverage:
`scripts/release/tests/test-check-release-bot-secrets.sh` (stubbed
`gh`).
