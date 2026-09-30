#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# Shared predicate: "is this pull request the machine-generated release PR?"
#
# ADR-1151 / ADR-1388. The six process gates in .github/workflows/rule-enforcement.yml
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
# doc-substance, docs/state.md touch, ffmpeg-patch surface sync — plus the
# Silent-Revert Guard (ADR-1284). The two correctness gates stay armed on
# release PRs on purpose: `Release Script Contract (ADR-1128)` is the gate that
# proves the cut ran, and `ADR Number Collision Guard` is diff-driven and
# trivially green when no ADR is added.
#
# Identity and diff verification (ADR-1151, ADR-1388):
# A human can push a branch called `release-please--anything`, so the head ref
# alone must not disarm a required gate.
# 1. Bot author: when the PR is authored by a bot (release-please running as the
#    release-bot GitHub App installation or `github-actions[bot]`), it is exempt
#    under ADR-1151.
# 2. PAT fallback author (ADR-1388): when release-please runs in PAT mode
#    (authored by the repository maintainer account such as `lusoris`), the PR
#    is exempt ONLY IF its diff is verified to touch strictly allowed
#    release-please files (manifest, config, changelog, and coordinated version
#    markers). Any diff touching other files keeps the gates armed.
#
# Usage (workflow):
#   env:
#     HEAD_REF:        ${{ github.event.pull_request.head.ref }}
#     PR_AUTHOR:       ${{ github.event.pull_request.user.login }}
#     PR_AUTHOR_TYPE:  ${{ github.event.pull_request.user.type }}
#     BASE_SHA:        ${{ github.event.pull_request.base.sha }}
#     HEAD_SHA:        ${{ github.event.pull_request.head.sha }}
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
base_sha="${BASE_SHA:-}"
head_sha="${HEAD_SHA:-}"
diff_file="${DIFF_FILE:-}"
pat_user="${RELEASE_BOT_PAT_USER:-lusoris}"

while [ $# -gt 0 ]; do
  case "$1" in
    --branch)
      head_ref="${2:-}"
      shift 2
      ;;
    --author)
      pr_author="${2:-}"
      shift 2
      ;;
    --author-type)
      pr_author_type="${2:-}"
      shift 2
      ;;
    --base)
      base_sha="${2:-}"
      shift 2
      ;;
    --head)
      head_sha="${2:-}"
      shift 2
      ;;
    --diff | --diff-file)
      diff_file="${2:-}"
      shift 2
      ;;
    --pat-user)
      pat_user="${2:-}"
      shift 2
      ;;
    *)
      shift
      ;;
  esac
done

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

# Locate coordinated version markers from release-please-config.json
repo_root="$(git rev-parse --show-toplevel 2>/dev/null || true)"
config_file=""
if [ -f "release-please-config.json" ]; then
  config_file="release-please-config.json"
elif [ -n "${repo_root}" ] && [ -f "${repo_root}/release-please-config.json" ]; then
  config_file="${repo_root}/release-please-config.json"
fi

extra_files=()
if [ -n "${config_file}" ] && command -v jq >/dev/null 2>&1; then
  mapfile -t extra_files < <(
    jq -r '.packages["."]."extra-files"[]? | if type == "string" then . else .path end' "${config_file}" 2>/dev/null || true
  )
fi

if [ "${#extra_files[@]}" -eq 0 ]; then
  extra_files=(
    "core/meson.build"
    "compat/python-vmaf/__init__.py"
    "ai/pyproject.toml"
    "ai/src/vmaf_train/__init__.py"
    "dev-llm/pyproject.toml"
    "dev-llm/src/vmaf_dev_llm/__init__.py"
    "mcp-server/vmaf-mcp/pyproject.toml"
    "mcp-server/vmaf-mcp/src/vmaf_mcp/__init__.py"
    "deploy/helm/vmafx/Chart.yaml"
    "build-config.env"
  )
fi

is_allowed_release_path() {
  local p="${1#./}"
  case "$p" in
    .release-please-manifest.json | \
      release-please-config.json | \
      CHANGELOG.md | \
      changelog.d/* | \
      docs/changelog-archive/*)
      return 0
      ;;
  esac

  local ef
  for ef in "${extra_files[@]}"; do
    if [ "$p" = "$ef" ]; then
      return 0
    fi
  done

  return 1
}

# Collect changed paths for diff inspection if requested or available
collect_changed_paths() {
  if [ -n "${diff_file}" ]; then
    if [ "${diff_file}" = "-" ]; then
      while IFS= read -r line || [ -n "$line" ]; do
        line="${line#"${line%%[![:space:]]*}"}"
        line="${line%"${line##*[![:space:]]}"}"
        [ -n "$line" ] && printf '%s\n' "$line"
      done
    elif [ -f "${diff_file}" ]; then
      while IFS= read -r line || [ -n "$line" ]; do
        line="${line#"${line%%[![:space:]]*}"}"
        line="${line%"${line##*[![:space:]]}"}"
        [ -n "$line" ] && printf '%s\n' "$line"
      done <"${diff_file}"
    fi
  elif [ -n "${base_sha}" ] && [ -n "${head_sha}" ]; then
    git cat-file -e "${base_sha}^{commit}" 2>/dev/null ||
      git fetch --no-tags origin "${base_sha}" 2>/dev/null || true
    git cat-file -e "${head_sha}^{commit}" 2>/dev/null ||
      git fetch --no-tags origin "${head_sha}" 2>/dev/null || true
    local diff_base
    diff_base="$(git merge-base "${base_sha}" "${head_sha}" 2>/dev/null || printf '%s' "${base_sha}")"
    git diff --name-only "${diff_base}..${head_sha}" 2>/dev/null || true
  elif git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    if git rev-parse --verify origin/master >/dev/null 2>&1; then
      local diff_base
      diff_base="$(git merge-base origin/master HEAD 2>/dev/null || true)"
      if [ -n "${diff_base}" ]; then
        git diff --name-only "${diff_base}..HEAD" 2>/dev/null || true
      fi
    fi
  fi
}

if [ "${release_branch}" = "true" ] && [ "${bot_author}" = "true" ]; then
  exempt="true"
  reason="machine-generated release PR (head ref '${head_ref}', author '${pr_author:-<unknown>}') — ADR-1151 exempts the authoring-discipline gates"
elif [ "${release_branch}" = "true" ] && [ -n "${pr_author}" ] && [ "${pr_author}" = "${pat_user}" ]; then
  # PAT-mode release PR (ADR-1388): verify that the diff contains only release files
  mapfile -t changed_paths < <(collect_changed_paths)
  if [ "${#changed_paths[@]}" -eq 0 ]; then
    exempt="false"
    reason="head ref '${head_ref}' looks like a release branch but author '${pr_author}' is not a bot (PAT release diff not satisfied: no changed files detected) — gates stay armed"
  else
    first_disallowed=""
    for p in "${changed_paths[@]}"; do
      [ -z "$p" ] && continue
      if ! is_allowed_release_path "$p"; then
        first_disallowed="$p"
        break
      fi
    done

    if [ -n "$first_disallowed" ]; then
      exempt="false"
      reason="head ref '${head_ref}' looks like a release branch but author '${pr_author}' is not a bot (PAT release diff not satisfied: touches non-release file '${first_disallowed}') — gates stay armed"
    else
      exempt="true"
      reason="machine-generated release PR in PAT mode (head ref '${head_ref}', author '${pr_author}') — ADR-1388 verified diff touches only release markers and changelog"
    fi
  fi
elif [ "${release_branch}" = "true" ]; then
  reason="head ref '${head_ref}' looks like a release branch but author '${pr_author:-<unknown>}' is not a bot — gates stay armed"
else
  reason="ordinary pull request — gates stay armed"
fi

if [ -n "${GITHUB_OUTPUT:-}" ]; then
  echo "exempt=${exempt}" >>"${GITHUB_OUTPUT}"
fi

echo "release-pr-exempt: exempt=${exempt} (${reason})"
