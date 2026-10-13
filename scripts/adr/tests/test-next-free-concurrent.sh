#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# scripts/adr/tests/test-next-free-concurrent.sh
#
# Concurrency test for `next-free.sh --claim`: many claims started at once,
# half from the repository root and half from docs/, must each get their own
# number. Before the fix the lock key depended on the caller's working
# directory, so a root claim and a docs/ claim held different locks and could
# write the same number.
#
# Runs in a throw-away repository so it cannot touch real ADR state.
#
# Usage:   bash scripts/adr/tests/test-next-free-concurrent.sh [CLAIMS] [ROUNDS]
# Exit:    0 = every round gave distinct numbers; 1 = a duplicate or empty claim.

set -euo pipefail

# shellcheck source=/dev/null
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/../../lib/clean-git-env.sh"

REPO_ROOT="$(git rev-parse --show-toplevel)"
SCRIPT="${REPO_ROOT}/scripts/adr/next-free.sh"
CLAIMS="${1:-12}"
ROUNDS="${2:-5}"

TMPDIR_BASE="$(mktemp -d)"
FAKE_REPO="${TMPDIR_BASE}/fakerepo"
mkdir -p "${FAKE_REPO}/docs/adr"
git -C "${FAKE_REPO}" init --quiet
touch "${FAKE_REPO}/docs/adr/0600-alpha.md"
git -C "${FAKE_REPO}" add .
git -C "${FAKE_REPO}" -c user.name=t -c user.email=t@example.org \
  commit --quiet -m "chore: initial ADR"
git -C "${FAKE_REPO}" remote add origin "${FAKE_REPO}"
git -C "${FAKE_REPO}" update-ref refs/remotes/origin/master \
  "$(git -C "${FAKE_REPO}" rev-parse HEAD)"

# Every lock name either version of the allocator can use for this repository.
_lock_dirs() {
  local d
  for d in .git ../.git "$(cd "${FAKE_REPO}/.git" && pwd -P)"; do
    printf '/tmp/vmaf_adr_claim_lock_%s\n' "$(printf '%s' "${d}" | tr '/' '_')"
  done
}

_cleanup() {
  local l
  while IFS= read -r l; do
    if [ -d "${l}" ] && [ -f "${l}/owner" ] && grep -qF "$(cd "${FAKE_REPO}" && pwd -P)" "${l}/owner" 2>/dev/null; then
      rm -f "${l}/owner"
      if ! rmdir "${l}" 2>/dev/null; then
        printf 'note: lock %s not removed\n' "${l}" >&2
      fi
    fi
  done < <(_lock_dirs)
  rm -rf "${TMPDIR_BASE}"
}
trap '_cleanup' EXIT INT TERM

FAIL=0
round=1
while [ "${round}" -le "${ROUNDS}" ]; do
  out_dir="${TMPDIR_BASE}/out-${round}"
  mkdir -p "${out_dir}"
  pids=()
  i=1
  while [ "${i}" -le "${CLAIMS}" ]; do
    if [ $((i % 2)) -eq 0 ]; then cwd="${FAKE_REPO}/docs"; else cwd="${FAKE_REPO}"; fi
    (cd "${cwd}" && bash "${SCRIPT}" --claim "conc-${round}-${i}" \
      >"${out_dir}/${i}" 2>"${out_dir}/${i}.err") &
    pids+=("$!")
    i=$((i + 1))
  done
  for p in "${pids[@]}"; do
    wait "${p}" || {
      printf 'FAIL: a claim exited non-zero in round %s\n' "${round}" >&2
      FAIL=1
    }
  done
  got="$(cat "${out_dir}"/[0-9]* | awk '/^[0-9][0-9][0-9][0-9]$/ { n++ } END { print n + 0 }')"
  uniq="$(cat "${out_dir}"/[0-9]* | grep -E '^[0-9]{4}$' | LC_ALL=C sort -u | wc -l)"
  if [ "${got}" -ne "${CLAIMS}" ] || [ "${uniq}" -ne "${CLAIMS}" ]; then
    printf 'FAIL: round %s: %s claims, %s numbers, %s distinct: %s\n' "${round}" \
      "${CLAIMS}" "${got}" "${uniq}" "$(cat "${out_dir}"/[0-9]* | tr '\n' ' ')" >&2
    FAIL=1
  else
    printf 'PASS: round %s: %s concurrent claims, %s distinct numbers\n' "${round}" "${CLAIMS}" "${uniq}"
  fi
  stubs="$(find "${FAKE_REPO}/docs/adr" -name '*.md.stub' | wc -l)"
  pointers="$(find "${FAKE_REPO}/.git/adr-claims" -type f 2>/dev/null | wc -l)"
  if [ "${stubs}" -ne $((round * CLAIMS)) ] || [ "${pointers}" -ne $((round * CLAIMS)) ]; then
    printf 'FAIL: round %s: %s stubs, %s claim pointers, expected %s each\n' "${round}" \
      "${stubs}" "${pointers}" $((round * CLAIMS)) >&2
    FAIL=1
  fi
  round=$((round + 1))
done

if [ "${FAIL}" -ne 0 ]; then
  printf 'FAILED: concurrent claims collided\n' >&2
  exit 1
fi
printf 'All concurrent-claim rounds passed.\n'
