#!/usr/bin/env bash
# scripts/ci/classify-dependency-pr.sh — classifies whether a PR is strictly
# dependency-only for automated gate exemption (ADR-1152).
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
#
# Exemption criteria (BOTH must hold):
#   (a) Author is `renovate[bot]` or `dependabot[bot]` (or `app/renovate` /
#       `app/dependabot`), OR the head branch matches `renovate/*` / `dependabot/*`.
#   (b) Every changed path is an allowed dependency manifest, lockfile, or
#       image-tag surface (container build files, Helm chart values/templates,
#       compose files, workflow pins).
#
# A Renovate PR that also edits source (e.g. under core/, ai/, python/, compat/,
# cmd/, pkg/, internal/, bindings/, tools/, scripts/, docs/, model/) must still
# be gated: that asymmetry is the whole point — bot PRs are only exempt when
# strictly updating dependency manifests or lockfiles with zero source edits.
#
# Usage:
#   scripts/ci/classify-dependency-pr.sh [--author LOGIN] [--branch REF] [--diff PATH]
#
# Environment variables:
#   PR_AUTHOR   Author username (e.g. github.event.pull_request.user.login)
#   HEAD_REF    Head branch ref (e.g. github.event.pull_request.head.ref)
#   BASE_SHA    Base commit SHA for git diff
#   HEAD_SHA    Head commit SHA for git diff
#   DIFF_FILE   Path to pre-computed list of changed paths (or stdin if '-')
#
# Exit codes:
#   0  PR is classified as dependency-only (exempt).
#   1  PR is NOT dependency-only (must be gated).
#   2  Usage or environment error.

set -euo pipefail

usage() {
  cat >&2 <<USAGE_EOF
Usage: $0 [options]

Options:
  --author LOGIN       PR author (default: \$PR_AUTHOR or \$GITHUB_ACTOR)
  --branch REF         Head branch (default: \$HEAD_REF or \$PR_BRANCH)
  --base SHA           Base commit SHA for git diff (default: \$BASE_SHA)
  --head SHA           Head commit SHA for git diff (default: \$HEAD_SHA)
  --diff PATH          Path to file listing changed files, one per line (or '-' for stdin)
  --paths-file PATH    Alias for --diff
  -h, --help           Show this help message
USAGE_EOF
}

author="${PR_AUTHOR:-${GITHUB_ACTOR:-}}"
branch="${HEAD_REF:-${PR_BRANCH:-}}"
base_sha="${BASE_SHA:-}"
head_sha="${HEAD_SHA:-}"
diff_file="${DIFF_FILE:-}"

while [ $# -gt 0 ]; do
  case "$1" in
    --author)
      author="${2:-}"
      shift 2
      ;;
    --branch)
      branch="${2:-}"
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
    --diff | --paths-file)
      diff_file="${2:-}"
      shift 2
      ;;
    -h | --help)
      usage
      exit 0
      ;;
    *)
      echo "classify-dependency-pr: unrecognized argument '$1'" >&2
      usage
      exit 2
      ;;
  esac
done

is_dependency_author_or_branch() {
  local auth="${1:-}"
  local br="${2:-}"

  case "$auth" in
    "renovate[bot]" | "dependabot[bot]" | "app/renovate" | "app/dependabot")
      return 0
      ;;
  esac

  case "$br" in
    renovate/* | dependabot/*)
      return 0
      ;;
  esac

  return 1
}

is_allowed_dependency_path() {
  local path="${1#./}"
  local base
  base="$(basename "$path")"

  # Prefix or exact full-path matches
  #
  # deploy/helm/* is the Helm chart surface. Renovate's helm-values and
  # docker-image managers pin container image tags inside values.yaml and the
  # chart templates, which is how `deploy/helm/vmafx/values.yaml` and
  # `deploy/helm/vmafx/templates/tests/test-connection.yaml` came to be the
  # only two paths in 25 consecutive Renovate PRs that this allowlist missed
  # (PR #1232 was blocked by exactly that gap). Whole-prefix granularity
  # matches how docker/* and .github/workflows/* are already treated: the
  # conjunction with condition (a) means only a bot author or bot branch can
  # reach this, so a human editing chart logic is still fully gated.
  case "$path" in
    renovate.json | \
      .github/renovate.json* | \
      .pre-commit-config.yaml | \
      dev/Containerfile | \
      docker/* | \
      deploy/helm/* | \
      .github/workflows/* | \
      changelog.d/*)
      return 0
      ;;
  esac

  # Basename matches for manifests and lockfiles across the tree
  case "$base" in
    renovate.json | \
      package.json | package-lock.json | pnpm-lock.yaml | yarn.lock | \
      go.mod | go.sum | \
      Cargo.toml | Cargo.lock | deny.toml | \
      pyproject.toml | poetry.lock | uv.lock | tox.ini | \
      requirements*.txt | constraints*.txt | \
      Dockerfile* | *.Dockerfile | \
      Chart.yaml | Chart.lock | \
      docker-compose.y*ml | docker-compose.*.y*ml | compose.y*ml)
      return 0
      ;;
  esac

  return 1
}

# 1. Collect changed paths
tmp_paths="$(mktemp)"
trap 'rm -f "$tmp_paths"' EXIT

if [ -n "$diff_file" ]; then
  if [ "$diff_file" = "-" ]; then
    cat >"$tmp_paths"
  else
    if [ ! -f "$diff_file" ]; then
      echo "classify-dependency-pr: diff file '$diff_file' not found" >&2
      exit 2
    fi
    cp "$diff_file" "$tmp_paths"
  fi
else
  if [ -n "$base_sha" ] && [ -n "$head_sha" ]; then
    git cat-file -e "${base_sha}^{commit}" 2>/dev/null ||
      git fetch --no-tags origin "${base_sha}" 2>/dev/null || true
    git cat-file -e "${head_sha}^{commit}" 2>/dev/null ||
      git fetch --no-tags origin "${head_sha}" 2>/dev/null || true
    # Use the MERGE BASE of the two, not base_sha itself.
    #
    # GitHub's `pull_request.base.sha` is the base branch tip as it was when the
    # PR was created (or last synchronised), not the current merge base. Once
    # master moves — which on this repo is constantly — a plain two-dot
    # `base_sha..head_sha` diff reports every file merged into master since the
    # branch point ON TOP OF the PR's own change.
    #
    # For dependency PRs that is fatal: a Renovate PR touching only
    # `deploy/helm/vmafx/values.yaml` was reported as touching 36 files
    # including `core/src/feature/*.c`, so this script concluded "bot PR touches
    # source code" and refused the exemption. Every dependency PR whose base had
    # moved failed the documentation gates for that reason alone.
    #
    # `git merge-base` gives the real fork point, which is what three-dot diff
    # syntax means. The fallback branch below already did this correctly; only
    # this explicit-SHA path was wrong.
    diff_base="$(git merge-base "${base_sha}" "${head_sha}" 2>/dev/null || printf '%s' "${base_sha}")"
    diff_head="${head_sha}"
  else
    if ! git rev-parse --verify origin/master >/dev/null 2>&1; then
      echo "classify-dependency-pr: origin/master not found; run 'git fetch origin master' first." >&2
      exit 2
    fi
    diff_base="$(git merge-base origin/master HEAD)"
    diff_head="HEAD"
  fi
  git diff --name-only "${diff_base}..${diff_head}" >"$tmp_paths"
fi

changed_paths=()
while IFS= read -r line || [ -n "$line" ]; do
  # Trim whitespace
  line="$(echo "$line" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')"
  [ -z "$line" ] && continue
  changed_paths+=("$line")
done <"$tmp_paths"

# If no branch was explicitly given and not in git env, try reading current branch
if [ -z "$branch" ]; then
  branch="$(git rev-parse --abbrev-ref HEAD 2>/dev/null || true)"
fi

# 2. Check author / branch requirement (Condition a)
author_branch_ok=0
if is_dependency_author_or_branch "$author" "$branch"; then
  author_branch_ok=1
fi

# 3. Check paths requirement (Condition b)
if [ "${#changed_paths[@]}" -eq 0 ]; then
  echo "classify-dependency-pr: NOT exempt — no changed paths detected."
  if [ -n "${GITHUB_OUTPUT:-}" ]; then
    echo "exempt=false" >>"$GITHUB_OUTPUT"
    echo "is_dependency_pr=false" >>"$GITHUB_OUTPUT"
  fi
  exit 1
fi

allowed_paths=()
disallowed_paths=()

for p in "${changed_paths[@]}"; do
  if is_allowed_dependency_path "$p"; then
    allowed_paths+=("$p")
  else
    disallowed_paths+=("$p")
  fi
done

if [ "$author_branch_ok" -ne 1 ]; then
  echo "classify-dependency-pr: NOT exempt — author '${author:-unknown}' and branch '${branch:-unknown}' do not match bot patterns (renovate[bot], dependabot[bot], renovate/*, dependabot/*)."
  if [ -n "${GITHUB_OUTPUT:-}" ]; then
    echo "exempt=false" >>"$GITHUB_OUTPUT"
    echo "is_dependency_pr=false" >>"$GITHUB_OUTPUT"
  fi
  exit 1
fi

if [ "${#disallowed_paths[@]}" -gt 0 ]; then
  echo "classify-dependency-pr: NOT exempt — ${#disallowed_paths[@]} path(s) are not allowed dependency manifests or lockfiles:"
  for p in "${disallowed_paths[@]}"; do
    echo "  - $p"
  done
  echo "Note: bot PRs that touch source code must still satisfy documentation gates."
  if [ -n "${GITHUB_OUTPUT:-}" ]; then
    echo "exempt=false" >>"$GITHUB_OUTPUT"
    echo "is_dependency_pr=false" >>"$GITHUB_OUTPUT"
  fi
  exit 1
fi

# BOTH conditions satisfied: exempt!
if [ "${#allowed_paths[@]}" -le 10 ]; then
  paths_summary="$(printf '%s, ' "${allowed_paths[@]}" | sed 's/, $//')"
else
  paths_summary="$(printf '%s, ' "${allowed_paths[@]:0:10}")... (${#allowed_paths[@]} files total)"
fi

echo "::notice title=Dependency-Only PR Exemption::PR is classified as dependency-only (author: '${author}', branch: '${branch}'); exempt from documentation gates. Changed paths: ${paths_summary}"

echo "classify-dependency-pr: EXEMPT — PR is strictly dependency-only."
echo "  Author: ${author}"
echo "  Branch: ${branch}"
echo "  Changed files (${#allowed_paths[@]}):"
for p in "${allowed_paths[@]}"; do
  echo "    - $p"
done

if [ -n "${GITHUB_OUTPUT:-}" ]; then
  echo "exempt=true" >>"$GITHUB_OUTPUT"
  echo "is_dependency_pr=true" >>"$GITHUB_OUTPUT"
fi

exit 0
