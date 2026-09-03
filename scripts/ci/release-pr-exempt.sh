#!/usr/bin/env bash
# Shared predicate: "is this pull request the machine-generated release PR?"
#
# ADR-1151. The six process gates in .github/workflows/rule-enforcement.yml
# became *required* contexts, which is correct for human PRs and wrong for the
# one PR nobody writes by hand. release-please generates the release PR from a
# template: its body is the rendered changelog (no ADR-0108 checklist, no
# `no docs needed:` sentinel), and its diff is `.release-please-manifest.json`
# plus the coordinated version markers — several of which are path-mapped to a
# docs/ tree the bot has no reason to touch (`mcp-server/vmaf-mcp/pyproject.toml`
# -> `docs/mcp/`). Without this exemption every release PR would carry a red
# required check and could only land through the admin bypass that ADR-1151
# exists to remove.
#
# Scope: the four *authoring-discipline* gates — deliverables checklist,
# doc-substance, docs/state.md touch, ffmpeg-patch surface sync. The two
# correctness gates stay armed on release PRs on purpose: `Release Script
# Contract (ADR-1128)` is the gate that proves the cut ran, and `ADR Number
# Collision Guard` is diff-driven and trivially green when no ADR is added.
#
# Identity, not branch name: a human can push a branch called
# `release-please--anything`, so the head ref alone must not disarm a required
# gate. The exemption additionally requires the PR to be authored by a bot
# account (release-please posts as the release-bot GitHub App installation, or
# as `github-actions[bot]` before that App exists).
#
# Usage (workflow):
#   env:
#     HEAD_REF:        ${{ github.event.pull_request.head.ref }}
#     PR_AUTHOR:       ${{ github.event.pull_request.user.login }}
#     PR_AUTHOR_TYPE:  ${{ github.event.pull_request.user.type }}
#   run: bash scripts/ci/release-pr-exempt.sh
#
# Usage (local dry run):
#   HEAD_REF=release-please--branches--master--components--vmafx \
#   PR_AUTHOR='github-actions[bot]' PR_AUTHOR_TYPE=Bot \
#     bash scripts/ci/release-pr-exempt.sh
#
# Always exits 0. Writes `exempt=true|false` to $GITHUB_OUTPUT when set, and
# always prints `exempt=<value>` plus the reason to stdout.

set -euo pipefail

head_ref="${HEAD_REF:-}"
pr_author="${PR_AUTHOR:-}"
pr_author_type="${PR_AUTHOR_TYPE:-}"

exempt="false"
reason=""

# release-please's branch naming is fixed:
#   release-please--branches--<base>--components--<package-name>
release_branch="false"
case "${head_ref}" in
  release-please--*) release_branch="true" ;;
esac

# GitHub reports App/bot authors as type `Bot`; the login always ends in
# `[bot]`. Accept either signal so the check survives a payload that omits
# `user.type`.
bot_author="false"
if [ "${pr_author_type}" = "Bot" ]; then
  bot_author="true"
else
  case "${pr_author}" in
    *'[bot]') bot_author="true" ;;
  esac
fi

if [ "${release_branch}" = "true" ] && [ "${bot_author}" = "true" ]; then
  exempt="true"
  reason="machine-generated release PR (head ref '${head_ref}', author '${pr_author:-<unknown>}') — ADR-1151 exempts the authoring-discipline gates"
elif [ "${release_branch}" = "true" ]; then
  reason="head ref '${head_ref}' looks like a release branch but author '${pr_author:-<unknown>}' is not a bot — gates stay armed"
else
  reason="ordinary pull request — gates stay armed"
fi

if [ -n "${GITHUB_OUTPUT:-}" ]; then
  echo "exempt=${exempt}" >>"${GITHUB_OUTPUT}"
fi

echo "release-pr-exempt: exempt=${exempt} (${reason})"
