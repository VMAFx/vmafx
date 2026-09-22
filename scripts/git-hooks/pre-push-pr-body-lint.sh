#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

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
#   - Bounds `gh` lookup time and falls back to GitHub's public PR pages.
#   - No-op if HEAD has no open PR with this branch as head.
#   - No-op if the PR is a draft (CI's deep-dive-checklist skips drafts).
#   - Otherwise: fetches the PR body via `gh pr view`, computes the diff
#     via `git diff --name-only origin/master..HEAD`, and runs
#     scripts/ci/validate-pr-body.sh. Non-zero exit blocks the push.
#   - Fails closed if neither metadata path can determine PR state.
#
# See: docs/development/pr-body-sentinel-guide.md

set -euo pipefail

lookup_timeout="${VMAFX_PR_LOOKUP_TIMEOUT_SECONDS:-15}"
case "${lookup_timeout}" in
  '' | *[!0-9]* | 0)
    echo "pre-push-pr-body-lint: lookup timeout must be a positive integer." >&2
    exit 1
    ;;
esac

temporary_paths=()
path=""
trap 'status=$?; trap - EXIT INT TERM; for path in "${temporary_paths[@]}"; do rm -rf -- "${path}"; done; exit "${status}"' EXIT INT TERM

run_gh_lookup() {
  local branch_name="$1"
  local output_path="$2"
  local command_pid
  local status=0
  local watchdog_pid

  gh pr view "${branch_name}" --json body,state,isDraft >"${output_path}" 2>/dev/null &
  command_pid=$!
  (
    sleep "${lookup_timeout}"
    kill -TERM "${command_pid}" 2>/dev/null || true
  ) &
  watchdog_pid=$!
  wait "${command_pid}" || status=$?
  kill -TERM "${watchdog_pid}" 2>/dev/null || true
  wait "${watchdog_pid}" 2>/dev/null || true
  return "${status}"
}

extract_pull_number() {
  local input_path="$1"
  local repository="$2"
  python3 - "${input_path}" "${repository}" <<'PY'
import re
import sys
from html.parser import HTMLParser


class PullLinks(HTMLParser):
    def __init__(self, repository):
        super().__init__()
        self.pattern = re.compile(rf"^/{re.escape(repository)}/pull/([0-9]+)$")
        self.numbers = set()

    def handle_starttag(self, _tag, attributes):
        href = dict(attributes).get("href", "").split("?", 1)[0].split("#", 1)[0]
        match = self.pattern.fullmatch(href)
        if match:
            self.numbers.add(match.group(1))


parser = PullLinks(sys.argv[2])
with open(sys.argv[1], encoding="utf-8") as handle:
    parser.feed(handle.read())
if not parser.numbers:
    raise SystemExit(3)
if len(parser.numbers) != 1:
    raise SystemExit(2)
print(parser.numbers.pop())
PY
}

normalise_pull_page() {
  local input_path="$1"
  local output_path="$2"
  python3 - "${input_path}" >"${output_path}" <<'PY'
import json
import sys
from html.parser import HTMLParser


class PullPage(HTMLParser):
    def __init__(self):
        super().__init__()
        self.body = None
        self.draft = False
        self.open = None

    def handle_starttag(self, tag, attributes):
        values = dict(attributes)
        classes = values.get("class", "").split()
        if "js-pull-header-details" in classes:
            self.open = values.get("data-pull-is-open") == "true"
        if values.get("data-status") == "draft":
            self.draft = True
        if tag == "clipboard-copy" and values.get("role") == "menuitem":
            if self.body is None and "value" in values:
                self.body = values["value"]


parser = PullPage()
with open(sys.argv[1], encoding="utf-8") as handle:
    parser.feed(handle.read())
if parser.open is not True or parser.body is None:
    raise SystemExit(2)
json.dump({"body": parser.body, "state": "OPEN", "isDraft": parser.draft}, sys.stdout)
PY
}

lookup_with_public_web() {
  local branch_name="$1"
  local output_path="$2"
  local search_path="$3"
  local page_path="$4"
  local origin
  local pull_number
  local repository

  command -v curl >/dev/null 2>&1 || return 2
  origin="$(git remote get-url origin 2>/dev/null)" || return 2
  case "${origin}" in
    https://github.com/*) repository="${origin#https://github.com/}" ;;
    git@github.com:*) repository="${origin#git@github.com:}" ;;
    ssh://git@github.com/*) repository="${origin#ssh://git@github.com/}" ;;
    *) return 2 ;;
  esac
  repository="${repository%.git}"
  case "${repository}" in
    */*) ;;
    *) return 2 ;;
  esac
  curl --fail --silent --show-error --max-time "${lookup_timeout}" \
    --get --data-urlencode "q=is:pr is:open head:${branch_name}" \
    "https://github.com/${repository}/pulls" >"${search_path}" || return 2
  pull_number="$(extract_pull_number "${search_path}" "${repository}")" || return $?
  curl --fail --silent --show-error --max-time "${lookup_timeout}" \
    "https://github.com/${repository}/pull/${pull_number}" >"${page_path}" || return 2
  normalise_pull_page "${page_path}" "${output_path}"
}

validate_pr_json() {
  local input_path="$1"
  python3 - "${input_path}" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    pull = json.load(handle)
valid = (
    isinstance(pull, dict)
    and isinstance(pull.get("body"), str)
    and isinstance(pull.get("state"), str)
    and isinstance(pull.get("isDraft"), bool)
)
raise SystemExit(0 if valid else 1)
PY
}

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

branch="$(git rev-parse --abbrev-ref HEAD 2>/dev/null || true)"
if [ -z "${branch}" ] || [ "${branch}" = "HEAD" ]; then
  # Detached HEAD — no PR association possible.
  exit 0
fi

# Skip master / main — no PR body to validate.
if [ "${branch}" = "master" ] || [ "${branch}" = "main" ]; then
  exit 0
fi

temporary_directory="$(mktemp -d)"
temporary_paths+=("${temporary_directory}")
pr_path="${temporary_directory}/pull-request.json"
search_path="${temporary_directory}/pull-search.html"
page_path="${temporary_directory}/pull-page.html"

lookup_status=1
if command -v gh >/dev/null 2>&1 && run_gh_lookup "${branch}" "${pr_path}"; then
  lookup_status=0
elif lookup_with_public_web "${branch}" "${pr_path}" "${search_path}" "${page_path}"; then
  lookup_status=0
else
  lookup_status=$?
fi

if [ "${lookup_status}" -eq 3 ]; then
  echo "pre-push-pr-body-lint: no open PR for branch '${branch}' — skipping (will run on next push after PR opens)." >&2
  exit 0
fi
if [ "${lookup_status}" -ne 0 ] || ! validate_pr_json "${pr_path}"; then
  echo "pre-push-pr-body-lint: cannot determine PR state via gh or GitHub's public pages; blocking push." >&2
  exit 1
fi
pr_json="$(<"${pr_path}")"

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

tmp_diff="${temporary_directory}/changed-files.txt"
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
