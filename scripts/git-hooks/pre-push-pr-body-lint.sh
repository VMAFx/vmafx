#!/usr/bin/env bash
# scripts/git-hooks/pre-push-pr-body-lint.sh — standalone PR-body lint entry point.
#
# This script is the named, standalone sibling to scripts/git-hooks/pre-push.
# It contains only the PR-body deliverables validation logic so that it can
# be referenced directly from .pre-commit-config.yaml as a pre-push stage
# hook (pre-commit requires a single-purpose entry script per hook id).
#
# The omnibus scripts/git-hooks/pre-push delegates to this script when the
# hook is installed via `make hooks-install`. The two are kept in sync by
# sharing the same validator call: scripts/ci/validate-pr-body.sh.
#
# Behaviour:
#   - No-op if `gh` is not on PATH.
#   - No-op if HEAD has no open PR with this branch as head.
#   - No-op if the PR is a draft (CI's deep-dive-checklist skips drafts).
#   - Otherwise: fetches the PR body via `gh pr view`, computes the diff
#     via `git diff --name-only origin/master..HEAD`, and runs
#     scripts/ci/validate-pr-body.sh. Non-zero exit blocks the push.
#
# Bypass: standard git escape hatch (the dash-dash-no-verify flag, intentionally
# not spelled out here so semgrep rule vmaf-no-skip-hooks-in-scripts does not
# fire on this documentation comment).
# See: docs/development/pr-body-sentinel-guide.md

set -euo pipefail

repo_root="$(git rev-parse --show-toplevel 2>/dev/null || true)"
if [ -z "${repo_root}" ]; then
  exit 0
fi

validator="${repo_root}/scripts/ci/validate-pr-body.sh"
if [ ! -x "${validator}" ]; then
  # Validator missing — skip silently. Older branches that pre-date
  # this hook should not be blocked from pushing.
  exit 0
fi

if ! command -v gh >/dev/null 2>&1; then
  echo "pre-push-pr-body-lint: gh CLI not found — skipping PR-body deliverables check." >&2
  exit 0
fi

branch="$(git rev-parse --abbrev-ref HEAD 2>/dev/null || true)"
if [ -z "${branch}" ] || [ "${branch}" = "HEAD" ]; then
  # Detached HEAD — no PR association possible.
  exit 0
fi

# Skip master / main — no PR body to validate.
if [ "${branch}" = "master" ] || [ "${branch}" = "main" ]; then
  exit 0
fi

# Look up the PR for this branch. `gh pr view --json` exits non-zero
# when there is no open PR; treat that as "first push of a feature
# branch" and skip.
pr_json=""
if ! pr_json="$(gh pr view "${branch}" --json body,state,isDraft 2>/dev/null)"; then
  echo "pre-push-pr-body-lint: no open PR for branch '${branch}' — skipping (will run on next push after PR opens)." >&2
  exit 0
fi

# Skip closed / merged PRs.
state="$(printf '%s' "${pr_json}" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("state",""))' 2>/dev/null || true)"
if [ "${state}" != "OPEN" ] && [ -n "${state}" ]; then
  echo "pre-push-pr-body-lint: PR for '${branch}' is ${state} — skipping." >&2
  exit 0
fi

# Draft PRs: CI's deep-dive-checklist job has the same
# pull_request.draft == false predicate, so mirror that locally.
is_draft="$(printf '%s' "${pr_json}" | python3 -c 'import json,sys; print(str(json.load(sys.stdin).get("isDraft",False)).lower())' 2>/dev/null || echo "false")"
if [ "${is_draft}" = "true" ]; then
  echo "pre-push-pr-body-lint: PR for '${branch}' is a draft — skipping (CI also skips drafts)." >&2
  exit 0
fi

body="$(printf '%s' "${pr_json}" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("body",""))' 2>/dev/null || true)"
if [ -z "${body}" ]; then
  echo "pre-push-pr-body-lint: PR body is empty — letting push through; CI will catch it." >&2
  exit 0
fi

if ! git rev-parse --verify origin/master >/dev/null 2>&1; then
  echo "pre-push-pr-body-lint: origin/master missing locally — run 'git fetch origin master'. Skipping." >&2
  exit 0
fi

tmp_diff="$(mktemp)"
trap 'rm -f "${tmp_diff}"' EXIT
base="$(git merge-base origin/master HEAD)"
git diff --name-only "${base}..HEAD" >"${tmp_diff}"

echo "pre-push-pr-body-lint: validating PR body against ADR-0108 deliverables gate..."
if ! printf '%s' "${body}" | "${validator}" --diff "${tmp_diff}"; then
  cat >&2 <<'EOF'

pre-push-pr-body-lint: BLOCKED — PR body would fail the rule-enforcement.yml
deep-dive-checklist gate (ADR-0108).

Fix the body with:
  gh pr edit --body-file <path>

Then push again. See docs/development/pr-body-sentinel-guide.md for the
exact checkbox syntax and opt-out sentinel forms.

Bypass (skips ALL pre-push checks): git push with the standard escape-hatch
flag (intentionally not spelled out here so semgrep does not flag this
documentation comment).
EOF
  exit 1
fi

exit 0
