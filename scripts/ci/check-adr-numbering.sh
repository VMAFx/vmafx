#!/usr/bin/env bash
# scripts/ci/check-adr-numbering.sh — pre-commit / CI guard against ADR number collisions.
#
# Two checks are performed on every staged docs/adr/NNNN-*.md file:
#
#   1. LOCAL UNIQUENESS: no other file in the working tree shares the same NNNN.
#      Catches the case where two branches were cut from the same master tip and
#      both added an ADR with the same number.
#
#   2. HEADING CONSISTENCY: the "# ADR-NNNN:" heading inside the file must use
#      the same NNNN as the filename.
#
# Invocation (pre-commit passes staged file paths as arguments):
#   bash scripts/ci/check-adr-numbering.sh [file ...]
#
# Exit codes:
#   0 — all checks passed
#   1 — one or more collisions or heading mismatches detected
#
# This script is wired into .pre-commit-config.yaml as a local hook.
# The CI gate (rule-enforcement.yml, job adr-collision-check) provides the
# cross-branch race guard that the local hook cannot cover.
#
# See ADR-0386 (docs/adr/0386-adr-numbering-collision-prevention.md).

set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

# ── helpers ──────────────────────────────────────────────────────────────────

fail=0

_error() {
  echo "::error file=${1}::${2}" >&2
  echo "  ERROR: ${2}" >&2
  fail=1
}

# ── main ─────────────────────────────────────────────────────────────────────

# If no files were passed (e.g. a manual dry-run), scan all ADR files.
if [ "$#" -eq 0 ]; then
  set -- docs/adr/[0-9][0-9][0-9][0-9]-*.md
fi

for file in "$@"; do
  # Skip files that aren't under docs/adr/ — the hook may receive other staged
  # files if invoked with `pass_filenames: true` globally.
  if [[ "${file}" != docs/adr/[0-9][0-9][0-9][0-9]-*.md ]]; then
    continue
  fi

  # Extract 4-digit prefix from filename.
  base="$(basename "${file}")"
  num="${base:0:4}"

  # ── Check 1: local uniqueness ──────────────────────────────────────────────
  # Count all files in docs/adr/ that start with the same 4-digit prefix.
  # Use find so we catch files that aren't yet on disk via ls (e.g. renames).
  colliders=()
  while IFS= read -r candidate; do
    # Exclude the file under test from the collision list.
    [[ "${candidate}" == "${file}" ]] && continue
    colliders+=("${candidate}")
  done < <(find docs/adr -maxdepth 1 -name "${num}-*.md" 2>/dev/null | LC_ALL=C sort)

  if [ "${#colliders[@]}" -gt 0 ]; then
    _error "${file}" \
      "ADR number collision: ${file} shares prefix ${num} with: ${colliders[*]}." \
      >&2 || true
    echo "" >&2
    echo "  Run 'scripts/adr/next-free.sh' to pick a free number." >&2
    echo "  Then rename both the file and its '# ADR-${num}:' heading." >&2
    echo "" >&2
    fail=1
  fi

  # ── Check 2: heading consistency ───────────────────────────────────────────
  # The first non-blank line must be "# ADR-NNNN: ...".
  if [ -f "${file}" ]; then
    first_heading="$(grep -m1 '^# ADR-' "${file}" 2>/dev/null || true)"
    heading_num="$(printf '%s' "${first_heading}" | grep -oE 'ADR-[0-9]{4}' | head -1 | grep -oE '[0-9]{4}' || true)"

    if [ -z "${heading_num}" ]; then
      _error "${file}" \
        "ADR heading missing or malformed in ${file}: expected '# ADR-${num}: ...' as the first heading." >&2 || true
      fail=1
    elif [ "${heading_num}" != "${num}" ]; then
      _error "${file}" \
        "ADR heading/filename mismatch in ${file}: filename says ${num} but heading says ${heading_num}. Update the heading to match." >&2 || true
      fail=1
    fi
  fi
done

if [ "${fail}" -eq 1 ]; then
  echo "" >&2
  echo "ADR numbering check failed.  See errors above." >&2
  echo "Use 'scripts/adr/next-free.sh' to claim a collision-free number." >&2
  exit 1
fi

echo "ADR numbering check passed."
exit 0
