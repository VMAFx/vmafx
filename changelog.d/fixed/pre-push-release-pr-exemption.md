- Pushing the changelog cut to the release-please branch is no longer blocked
  by the local pre-push PR-body hook. The hook now applies CI's release-PR
  exemption (`scripts/ci/release-pr-exempt.sh`) to the bot release PR, and
  still validates human PRs and any PR whose head ref is not the pushed branch.
