#!/usr/bin/env bash
# check-release-bot-secrets.sh — ADR-1151 / ADR-1171 preflight.
#
# release-please.yml authenticates as the release-bot GitHub App. Until the two
# repository secrets exist the workflow stays idle on master (warning, not
# failure — ADR-1171), so nothing on the run colour tells an operator that a
# release cannot be cut. This script is that signal: it lists the repository's
# secret NAMES through `gh secret list` (values are never readable) and fails
# when either name is absent. It is part of the /prep-release dry run.
#
# Usage: scripts/release/check-release-bot-secrets.sh [OWNER/REPO]
# Exit: 0 both present; 1 one or both missing; 2 gh unavailable / not authorised.
set -euo pipefail

repo="${1:-VMAFx/vmafx}"
required=(RELEASE_BOT_APP_ID RELEASE_BOT_PRIVATE_KEY)

if ! command -v gh >/dev/null 2>&1; then
  echo "check-release-bot-secrets: gh is not installed" >&2
  exit 2
fi
if ! names="$(gh secret list -R "$repo" --json name --jq '.[].name' 2>/dev/null)"; then
  echo "check-release-bot-secrets: cannot list secrets of $repo (needs repo admin)" >&2
  exit 2
fi

missing=()
for n in "${required[@]}"; do
  grep -qx "$n" <<<"$names" || missing+=("$n")
done
if [[ ${#missing[@]} -gt 0 ]]; then
  echo "check-release-bot-secrets: $repo is missing: ${missing[*]}" >&2
  echo "Create the release-bot App and add the secrets — docs/development/release.md," >&2
  echo "section 'Release-bot identity' (ADR-1151, ADR-1171)." >&2
  exit 1
fi
echo "check-release-bot-secrets: ${required[*]} present on $repo"
