#!/usr/bin/env bash
# scripts/ci/validate-pr-body.sh — local PR-body deliverables validator.
#
# Closes the loop where the strict deliverables-checklist parser at
# .github/workflows/rule-enforcement.yml repeatedly tripped contributors
# on `gh pr edit` retries (see PRs #461, #438, #470, #473, #486, #511,
# #468, #526). Every retry costs a CI cycle (~3-10 min). This validator
# runs the SAME parser locally before push.
#
# IMPORTANT: This validator passing is NOT a guarantee that the CI gate
# will pass. CI is authoritative. The validator re-uses
# scripts/ci/deliverables-check.sh's parser verbatim, so the two should
# agree on parser-shape failures, but CI's environment (BASE_SHA /
# HEAD_SHA from the PR object) may diverge from the local
# `git diff origin/master..HEAD` heuristic the hook uses.
#
# Usage:
#   # Body from stdin, diff from a file (one path per line):
#   git diff --name-only origin/master..HEAD > /tmp/diff.txt
#   gh pr view 260 --json body -q .body \
#       | scripts/ci/validate-pr-body.sh --diff /tmp/diff.txt
#
#   # Body from a markdown file:
#   scripts/ci/validate-pr-body.sh --body pr-body.md --diff /tmp/diff.txt
#
#   # No --diff: falls back to `git diff --name-only origin/master..HEAD`
#   gh pr view 260 --json body -q .body \
#       | scripts/ci/validate-pr-body.sh
#
# Exits:
#   0  PR body would pass the deliverables-check gate.
#   1  PR body would fail the gate (same error message + line number
#      as the CI gate emits).
#   2  Usage error (no body supplied, missing --diff file, etc.).

set -euo pipefail

usage() {
  cat >&2 <<EOF
Usage: $0 [--body PATH] [--diff PATH]

Options:
  --body PATH   Read PR body from PATH (default: read from stdin).
  --diff PATH   Read changed-file paths from PATH, one per line
                (default: \`git diff --name-only origin/master..HEAD\`).
  -h, --help    Show this help.

Validates a PR body against the ADR-0108 six-deliverable gate enforced
by .github/workflows/rule-enforcement.yml. Re-uses the parser in
scripts/ci/deliverables-check.sh as the single source of truth.
EOF
}

body_path=""
diff_path=""

while [ $# -gt 0 ]; do
  case "$1" in
    --body)
      body_path="${2:-}"
      shift 2
      ;;
    --diff)
      diff_path="${2:-}"
      shift 2
      ;;
    -h | --help)
      usage
      exit 0
      ;;
    *)
      echo "validate-pr-body: unknown argument: $1" >&2
      usage
      exit 2
      ;;
  esac
done

# ---------- Resolve PR body source ----------

if [ -n "${body_path}" ]; then
  if [ ! -r "${body_path}" ]; then
    echo "validate-pr-body: cannot read body file: ${body_path}" >&2
    exit 2
  fi
  body_text="$(cat -- "${body_path}")"
elif [ ! -t 0 ]; then
  body_text="$(cat)"
else
  echo "validate-pr-body: no PR body supplied — pass --body PATH or pipe on stdin." >&2
  usage
  exit 2
fi

# ---------- Resolve diff source ----------

# deliverables-check.sh consumes the diff via $BASE_SHA..$HEAD_SHA env
# vars. To inject an arbitrary file list, we pre-compute the diff into
# a temp file and use a sentinel BASE/HEAD pair that the called script
# would resolve via `git diff` — which means we cannot just pass arb
# paths directly. Instead, we shadow `git` for the child process via a
# wrapper on PATH that returns the file list when the parser asks for
# `git diff --name-only`. Less invasive than monkey-patching the parser.

repo_root="$(git rev-parse --show-toplevel 2>/dev/null || true)"
if [ -z "${repo_root}" ]; then
  echo "validate-pr-body: not inside a git repository." >&2
  exit 2
fi

deliverables_script="${repo_root}/scripts/ci/deliverables-check.sh"
if [ ! -x "${deliverables_script}" ]; then
  echo "validate-pr-body: cannot execute ${deliverables_script}" >&2
  exit 2
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "${tmpdir}"' EXIT

if [ -n "${diff_path}" ]; then
  if [ ! -r "${diff_path}" ]; then
    echo "validate-pr-body: cannot read diff file: ${diff_path}" >&2
    exit 2
  fi
  cp -- "${diff_path}" "${tmpdir}/diff.txt"
else
  if ! git rev-parse --verify origin/master >/dev/null 2>&1; then
    echo "validate-pr-body: origin/master not found; run 'git fetch origin master' or pass --diff." >&2
    exit 2
  fi
  base="$(git merge-base origin/master HEAD)"
  git diff --name-only "${base}..HEAD" >"${tmpdir}/diff.txt"
fi

# ---------- Build a `git` shim that intercepts `diff --name-only` ----------
#
# deliverables-check.sh runs `git diff --name-only "${diff_base}..${diff_head}"`
# unconditionally. We override it via a PATH-shim so the parser sees our
# pre-computed file list. Every other git invocation falls through to the
# real binary so `rev-parse --verify origin/master` and friends still work.

real_git="$(command -v git)"
shim_dir="${tmpdir}/shim"
mkdir -p "${shim_dir}"

cat >"${shim_dir}/git" <<SHIM
#!/usr/bin/env bash
set -euo pipefail
if [ "\${1:-}" = "diff" ] && [ "\${2:-}" = "--name-only" ]; then
  cat -- "${tmpdir}/diff.txt"
  exit 0
fi
exec "${real_git}" "\$@"
SHIM
chmod +x "${shim_dir}/git"

# Sentinel BASE_SHA / HEAD_SHA values so the parser takes the env-var
# branch and skips the auto merge-base fallback (which would re-invoke
# `git merge-base` — fine via the shim, but the explicit branch is
# clearer).
PATH="${shim_dir}:${PATH}" \
  PR_BODY="${body_text}" \
  BASE_SHA="validator-base" \
  HEAD_SHA="validator-head" \
  bash "${deliverables_script}"
