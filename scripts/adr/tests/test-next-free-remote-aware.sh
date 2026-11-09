#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# scripts/adr/tests/test-next-free-remote-aware.sh
#
# Acceptance tests for the remote-aware ADR allocator (next-free.sh).
#
# Tests:
#   1. Remote branches with overlapping ADR files → allocator returns max+1.
#   2. Offline mode (mocked ls-remote failure) → local fallback + WARNING on stderr.
#   3. Sibling worktree claim via .git/adr-claims/ → second call returns higher number.
#   4. Performance smoke (must complete within 5 s on a synthetic-branch set).
#   5. Fork pull request head with ADR file → allocator returns number above PR ADR.
#   6. Negative: gh failure → fallback to branches/master with WARNING.
#   7. Negative: no gh on PATH → fallback with WARNING with reason.
#   8. Boundary: gh returns empty list → no warning, number as before.
#   9. Positive: gh returns 1000 SHAs (limit reached) → incomplete-scan WARNING.
#  10. Negative: gh returns 999 SHAs (< limit) → no incomplete-scan WARNING.
#  11. Boundary: gh prints 1 SHA + non-SHA stderr line, exit 0, TMPDIR missing.
#  12. Split fetch: branch ADRs survive absent PR head commit SHA.
#  13. VMAF_ADR_PR_REPO override: positive (custom repo), negative (unset), boundary (empty).
#
# All tests run in a throw-away temporary git repository so they cannot touch
# production ADR state.  The real next-free.sh is invoked via PATH override with
# a wrapper that intercepts 'git ls-remote' to return controlled output.
#
# Usage:
#   bash scripts/adr/tests/test-next-free-remote-aware.sh
#
# Exit code: 0 = all tests passed; non-zero = one or more failures.
#
# See ADR-0535 and ADR-NNNN (remote-aware allocator).

set -euo pipefail

# Drop the git hook environment and the fixture identity (see the helper).
# shellcheck source=/dev/null
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/../../lib/clean-git-env.sh"

REPO_ROOT="$(git rev-parse --show-toplevel)"
SCRIPT="${SCRIPT:-${REPO_ROOT}/scripts/adr/next-free.sh}"

# Resolve the real git binary path at test-script startup time so stubs can
# use it without going through PATH (which would recurse back into the stub).
REAL_GIT="$(command -v git)"

# ── test harness ──────────────────────────────────────────────────────────────

PASS=0
FAIL=0

_pass() {
  printf 'PASS: %s\n' "$1"
  PASS=$((PASS + 1))
}

_fail() {
  printf 'FAIL: %s\n' "$1" >&2
  FAIL=$((FAIL + 1))
}

# ── fixture: synthetic git repo ───────────────────────────────────────────────

# Create a temporary git repo that mimics the real repo structure.
# next-free.sh uses 'git rev-parse --show-toplevel' so we must run it inside
# a real git repo.  We override git commands via PATH-injected stubs.
TMPDIR_BASE="$(mktemp -d)"
FAKE_REPO="${TMPDIR_BASE}/fakerepo"
STUB_BIN="${TMPDIR_BASE}/stub-bin"
mkdir -p "${FAKE_REPO}/docs/adr" "${STUB_BIN}"

# Default gh stub: returns 0 with no PRs (tests override as needed).
cat >"${STUB_BIN}/gh" <<'GHEOF'
#!/usr/bin/env bash
exit 0
GHEOF
chmod +x "${STUB_BIN}/gh"

# Initialise the fake repo and create a minimal origin/master state.
git -C "${FAKE_REPO}" init --quiet
# Create a couple of ADR files that represent origin/master state.
touch "${FAKE_REPO}/docs/adr/0600-alpha.md"
touch "${FAKE_REPO}/docs/adr/0601-bravo.md"
git -C "${FAKE_REPO}" add .
git -C "${FAKE_REPO}" commit --quiet -m "chore: initial ADRs"
# Create a remote called 'origin' pointing to itself so ls-remote works.
git -C "${FAKE_REPO}" remote add origin "${FAKE_REPO}"

# Make origin/master point at HEAD.
git -C "${FAKE_REPO}" update-ref refs/remotes/origin/master \
  "$(git -C "${FAKE_REPO}" rev-parse HEAD)"

# Register cleanup.
_cleanup() {
  # Release any lock that a test may have left behind (before removing the dir).
  local gcd lkey
  # next-free.sh keys the lock on the absolute common .git directory.
  gcd="$(cd "${FAKE_REPO}/.git" && pwd -P)"
  lkey="$(printf '%s' "${gcd}" | tr '/' '_')"
  if [ -d "/tmp/vmaf_adr_claim_lock_${lkey}" ]; then
    rm -f "/tmp/vmaf_adr_claim_lock_${lkey}/owner"
    if ! rmdir "/tmp/vmaf_adr_claim_lock_${lkey}" 2>/dev/null; then
      printf 'note: lock %s not removed\n' "/tmp/vmaf_adr_claim_lock_${lkey}" >&2
    fi
  fi
  rm -rf "${TMPDIR_BASE}"
}
trap '_cleanup' EXIT INT TERM

# ── helper: run next-free.sh inside the fake repo ─────────────────────────────

# Run next-free.sh inside the fake repo with a custom PATH that injects
# stub commands.  All arguments are forwarded.
_run() {
  (cd "${FAKE_REPO}" && PATH="${STUB_BIN}:${PATH}" bash "${SCRIPT}" "$@")
}

# _git_common_dir: return the absolute path of the common git dir for FAKE_REPO.
# git rev-parse --git-common-dir can return a relative path (e.g. ".git"), so
# we prefix with the repo root to get a stable absolute path.
_git_common_dir() {
  local rel
  rel="$(git -C "${FAKE_REPO}" rev-parse --git-common-dir)"
  case "${rel}" in
    /*) printf '%s' "${rel}" ;;
    *) printf '%s/%s' "${FAKE_REPO}" "${rel}" ;;
  esac
}

# ── test 1: remote branches with overlapping ADR numbers ─────────────────────
#
# Scenario: origin has two in-flight branches that each contain an ADR file.
#   branch-a  →  docs/adr/0602-charlie.md   (tip SHA aaa...)
#   branch-b  →  docs/adr/0603-delta.md     (tip SHA bbb...)
# Master already has 0600 and 0601.
# Expected: allocator picks 0604 (max of {0600,0601,0602,0603} + 1).

# Create two commits that represent branch tips with ADR files.
git -C "${FAKE_REPO}" checkout --quiet -b branch-a
touch "${FAKE_REPO}/docs/adr/0602-charlie.md"
git -C "${FAKE_REPO}" add docs/adr/0602-charlie.md
git -C "${FAKE_REPO}" commit --quiet -m "feat: adr 0602"
SHA_A="$(git -C "${FAKE_REPO}" rev-parse HEAD)"

git -C "${FAKE_REPO}" checkout --quiet -b branch-b master
touch "${FAKE_REPO}/docs/adr/0603-delta.md"
git -C "${FAKE_REPO}" add docs/adr/0603-delta.md
git -C "${FAKE_REPO}" commit --quiet -m "feat: adr 0603"
SHA_B="$(git -C "${FAKE_REPO}" rev-parse HEAD)"

# Go back to a detached HEAD or a dummy branch so the script can read master.
git -C "${FAKE_REPO}" checkout --quiet master

# Register branch-a and branch-b as remote refs so git ls-remote finds them.
git -C "${FAKE_REPO}" update-ref "refs/remotes/origin/branch-a" "${SHA_A}"
git -C "${FAKE_REPO}" update-ref "refs/remotes/origin/branch-b" "${SHA_B}"

# Stub git ls-remote to return branch-a and branch-b.
# REAL_GIT is expanded here (at generation time) so the stub does not recurse
# back into itself when exec-ing the real git binary.
MASTER_SHA_T1="$(git -C "${FAKE_REPO}" rev-parse master)"
cat >"${STUB_BIN}/git" <<GITEOF
#!/usr/bin/env bash
# Intercept ls-remote calls; pass everything else to the real git.
if [ "\$1" = "ls-remote" ] && [ "\$2" = "--heads" ] && [ "\$3" = "origin" ]; then
  printf '%s\trefs/heads/branch-a\n' "${SHA_A}"
  printf '%s\trefs/heads/branch-b\n' "${SHA_B}"
  printf '%s\trefs/heads/master\n' "${MASTER_SHA_T1}"
  exit 0
fi
exec "${REAL_GIT}" "\$@"
GITEOF
chmod +x "${STUB_BIN}/git"

# Use --claim so the remote-branch scan path is exercised (query mode is
# local-only by design).
result_t1=$(_run --claim test-remote-t1 2>/dev/null) || echo "note: _run exited non-zero" >&2
_STUBS_T1=("${FAKE_REPO}/docs/adr/${result_t1}-test-remote-t1.md.stub")
if [ "${result_t1}" = "0604" ]; then
  _pass "test 1: remote branches with overlapping ADRs → allocator returns 0604 (max+1)"
else
  _fail "test 1: expected 0604, got '${result_t1}'"
fi

# Clean up stub, branch refs, and git stub.
rm -f "${_STUBS_T1[@]+"${_STUBS_T1[@]}"}"
GIT_COMMON_T1="$(_git_common_dir)"
rm -f "${GIT_COMMON_T1}/adr-claims/${result_t1}"
# Delete the remote-tracking refs AND the local branches so that subsequent
# git ls-remote origin calls (which hit the local-path remote and return its
# refs/heads/*) do not see branch-a / branch-b.
if git -C "${FAKE_REPO}" show-ref --verify --quiet "refs/remotes/origin/branch-a"; then
  git -C "${FAKE_REPO}" update-ref -d "refs/remotes/origin/branch-a"
fi
if git -C "${FAKE_REPO}" show-ref --verify --quiet "refs/remotes/origin/branch-b"; then
  git -C "${FAKE_REPO}" update-ref -d "refs/remotes/origin/branch-b"
fi
if git -C "${FAKE_REPO}" show-ref --verify --quiet "refs/heads/branch-a"; then
  git -C "${FAKE_REPO}" branch -D branch-a >/dev/null
fi
if git -C "${FAKE_REPO}" show-ref --verify --quiet "refs/heads/branch-b"; then
  git -C "${FAKE_REPO}" branch -D branch-b >/dev/null
fi
rm -f "${STUB_BIN}/git"

# ── test 2: offline mode → local fallback + WARNING on stderr ─────────────────
#
# Scenario: git ls-remote --heads fails (network unreachable) but git fetch
#   origin master still works (the two calls are independent).
# Expected: --claim still returns a valid number,
#           stderr contains "remote branch scan skipped".
#
# We stub ONLY the ls-remote --heads call; all other git commands run normally.

cat >"${STUB_BIN}/git" <<GITEOF
#!/usr/bin/env bash
# Fail git ls-remote --heads origin to simulate offline mode.
if [ "\$1" = "ls-remote" ] && [ "\$2" = "--heads" ]; then
  exit 1
fi
exec "${REAL_GIT}" "\$@"
GITEOF
chmod +x "${STUB_BIN}/git"

stderr_t2="$(mktemp)"
result_t2=$(_run --claim test-offline-t2 2>"${stderr_t2}") || echo "note: _run exited non-zero" >&2
GIT_COMMON_T2="$(_git_common_dir)"

if printf '%s' "${result_t2}" | grep -qE '^[0-9]{4}$'; then
  _pass "test 2a: offline mode still returns a 4-digit number: ${result_t2}"
else
  _fail "test 2a: offline mode returned non-numeric output: '${result_t2}'"
fi

if grep -q "remote branch scan skipped" "${stderr_t2}"; then
  _pass "test 2b: offline mode prints WARNING to stderr"
else
  _fail "test 2b: expected 'remote branch scan skipped' in stderr, got: $(cat "${stderr_t2}")"
fi

# Clean up stub, claim stub, and side-pointer.
rm -f "${FAKE_REPO}/docs/adr/${result_t2}-test-offline-t2.md.stub"
rm -f "${GIT_COMMON_T2}/adr-claims/${result_t2}"
rm -f "${stderr_t2}" "${STUB_BIN}/git"

# ── test 3: sibling worktree claim via .git/adr-claims/ ───────────────────────
#
# Scenario: a sibling agent (worktree sharing the same .git/) has already
#   written .git/adr-claims/0602.  The next --claim in this worktree must skip
#   0602 and return 0603 (since master only has 0600 and 0601).

GIT_COMMON="$(_git_common_dir)"
mkdir -p "${GIT_COMMON}/adr-claims"
printf 'sibling-slug 2026-05-19T00:00:00Z sibling/branch\n' \
  >"${GIT_COMMON}/adr-claims/0602"

result_t3=$(_run --claim test-sibling-t3 2>/dev/null) || echo "note: _run exited non-zero" >&2
if [ "${result_t3}" = "0603" ]; then
  _pass "test 3: sibling worktree claim in .git/adr-claims/ correctly blocked 0602 → returned 0603"
else
  _fail "test 3: expected 0603 (sibling claimed 0602), got '${result_t3}'"
fi

# Clean up side-pointers and stub.
rm -f "${GIT_COMMON}/adr-claims/0602"
rm -f "${GIT_COMMON}/adr-claims/${result_t3}"
rm -f "${FAKE_REPO}/docs/adr/${result_t3}-test-sibling-t3.md.stub"

# ── test 4: performance smoke ─────────────────────────────────────────────────
#
# Scenario: 30 remote branches, each with a docs/adr/ tree.
# The full --claim path must complete in < 5 seconds.
# We do NOT actually create 30 commits; we stub git ls-remote to return 30
# branch SHAs (all pointing at the existing master commit) and rely on
# git ls-tree being fast on a known small tree.

MASTER_SHA="$(git -C "${FAKE_REPO}" rev-parse master)"
# Build fake ls-remote output: 30 branches all pointing to master SHA.
# Written to a temp file and sourced by the stub to avoid quoting issues.
FAKE_LS_REMOTE_FILE="${TMPDIR_BASE}/fake-ls-remote.txt"
for i in $(seq 1 30); do
  printf '%s\trefs/heads/perf-branch-%02d\n' "${MASTER_SHA}" "${i}"
done >"${FAKE_LS_REMOTE_FILE}"
printf '%s\trefs/heads/master\n' "${MASTER_SHA}" >>"${FAKE_LS_REMOTE_FILE}"

cat >"${STUB_BIN}/git" <<GITEOF
#!/usr/bin/env bash
if [ "\$1" = "ls-remote" ] && [ "\$2" = "--heads" ] && [ "\$3" = "origin" ]; then
  cat "${FAKE_LS_REMOTE_FILE}"
  exit 0
fi
exec "${REAL_GIT}" "\$@"
GITEOF
chmod +x "${STUB_BIN}/git"

perf_start="$(date +%s%3N)"
result_t4=$(_run --claim test-perf-t4 2>/dev/null) || echo "note: _run exited non-zero" >&2
perf_end="$(date +%s%3N)"
elapsed_ms=$((perf_end - perf_start))

GIT_COMMON_T4="$(_git_common_dir)"
rm -f "${STUB_BIN}/git"
rm -f "${FAKE_REPO}/docs/adr/${result_t4}-test-perf-t4.md.stub"
rm -f "${GIT_COMMON_T4}/adr-claims/${result_t4}"

if [ "${elapsed_ms}" -lt 5000 ]; then
  _pass "test 4a: performance smoke completed in ${elapsed_ms} ms (< 5000 ms)"
else
  _fail "test 4a: performance smoke took ${elapsed_ms} ms (>= 5000 ms limit)"
fi

if printf '%s' "${result_t4}" | grep -qE '^[0-9]{4}$'; then
  _pass "test 4b: performance smoke returned valid 4-digit number: ${result_t4}"
else
  _fail "test 4b: performance smoke returned invalid output: '${result_t4}'"
fi

# ── test 5: fork pull request head with ADR file ──────────────────────────────
#
# Scenario: an open pull request from a fork has a head commit that contains
#   docs/adr/0700-fork.md. The commit is reachable only as refs/pull/1/head
#   in the fake origin (no branch on origin).
#   gh stub outputs the head commit SHA.
# Expected: allocator returns exactly 0701 (0700 + 1).

git -C "${FAKE_REPO}" checkout --quiet -b temp-fork-pr master
touch "${FAKE_REPO}/docs/adr/0700-fork.md"
git -C "${FAKE_REPO}" add docs/adr/0700-fork.md
git -C "${FAKE_REPO}" commit --quiet -m "feat: fork pr adr 0700"
SHA_FORK="$(git -C "${FAKE_REPO}" rev-parse HEAD)"
git -C "${FAKE_REPO}" checkout --quiet master
git -C "${FAKE_REPO}" branch -D temp-fork-pr >/dev/null
git -C "${FAKE_REPO}" update-ref "refs/pull/1/head" "${SHA_FORK}"

ORIGIN_URL="$(git -C "${FAKE_REPO}" remote get-url origin)"
cat >"${STUB_BIN}/gh" <<GHEOF
#!/usr/bin/env bash
has_repo=0
has_state_open=0
has_json_head=0
prev=""
for arg in "\$@"; do
  if [ "\${prev}" = "-R" ] && [ "\${arg}" = "${ORIGIN_URL}" ]; then
    has_repo=1
  fi
  if [ "\${prev}" = "--state" ] && [ "\${arg}" = "open" ]; then
    has_state_open=1
  fi
  if [ "\${prev}" = "--json" ] && [ "\${arg}" = "headRefOid" ]; then
    has_json_head=1
  fi
  prev="\${arg}"
done
if [ "\$1" != "pr" ] || [ "\$2" != "list" ]; then
  printf 'gh stub: expected pr list, got %s %s\n' "\$1" "\$2" >&2
  exit 1
fi
if [ "\${has_repo}" -ne 1 ]; then
  printf 'gh stub: missing or invalid -R argument (expected %s)\n' "${ORIGIN_URL}" >&2
  exit 1
fi
if [ "\${has_state_open}" -ne 1 ]; then
  printf 'gh stub: missing --state open\n' >&2
  exit 1
fi
if [ "\${has_json_head}" -ne 1 ]; then
  printf 'gh stub: missing --json headRefOid\n' >&2
  exit 1
fi
printf '%s\n' "${SHA_FORK}"
exit 0
GHEOF
chmod +x "${STUB_BIN}/gh"

result_t5=$(_run --claim test-fork-pr 2>/dev/null) || echo "note: _run exited non-zero" >&2
_STUBS_T5=("${FAKE_REPO}/docs/adr/${result_t5}-test-fork-pr.md.stub")
GIT_COMMON_T5="$(_git_common_dir)"

if [ "${result_t5}" = "0701" ]; then
  _pass "test 5a: fork PR head with ADR 0700 → allocator returns 0701"
else
  _fail "test 5a: expected 0701, got '${result_t5}'"
fi

# Negative: prove the strict gh stub rejects calls without --state open
if "${STUB_BIN}/gh" pr list -R "${ORIGIN_URL}" --state closed --json headRefOid 2>/dev/null; then
  _fail "test 5b: strict gh stub accepted --state closed"
else
  _pass "test 5b: strict gh stub rejects call without --state open"
fi

# Clean up stub, claim sidepointer, and refs/pull/1/head.
rm -f "${_STUBS_T5[@]+"${_STUBS_T5[@]}"}"
rm -f "${GIT_COMMON_T5}/adr-claims/${result_t5}"
git -C "${FAKE_REPO}" update-ref -d "refs/pull/1/head"

# ── test 6: negative: gh exits 1 → fallback to branches/master + WARNING ──────
#
# Scenario: gh stub exits 1 (simulating failure or not logged in).
# Expected: claim succeeds with exit 0, returns next free number (0602),
#           and stderr contains WARNING with reason text.

cat >"${STUB_BIN}/gh" <<'GHEOF'
#!/usr/bin/env bash
printf 'GraphQL: could not resolve to a Repository\n' >&2
exit 1
GHEOF
chmod +x "${STUB_BIN}/gh"

stderr_t6="$(mktemp)"
result_t6=$(_run --claim test-gh-fail-t6 2>"${stderr_t6}") || echo "note: _run exited non-zero" >&2
GIT_COMMON_T6="$(_git_common_dir)"

if [ "${result_t6}" = "0602" ]; then
  _pass "test 6a: gh failure returns expected number 0602"
else
  _fail "test 6a: expected 0602, got '${result_t6}'"
fi

if grep -q "WARNING: open pull request heads not scanned (GraphQL: could not resolve to a Repository); fork pull request ADR numbers may collide" "${stderr_t6}"; then
  _pass "test 6b: gh failure prints expected WARNING with reason text on stderr"
else
  _fail "test 6b: expected WARNING on stderr, got: $(cat "${stderr_t6}")"
fi

rm -f "${FAKE_REPO}/docs/adr/${result_t6}-test-gh-fail-t6.md.stub"
rm -f "${GIT_COMMON_T6}/adr-claims/${result_t6}"
rm -f "${stderr_t6}"

# ── test 7: negative: no gh on PATH → fallback + WARNING with reason ─────────
#
# Scenario: gh is completely missing from PATH.
# Expected: claim succeeds with exit 0, returns next free number (0602),
#           and stderr contains WARNING with reason.

NO_GH_BIN="${TMPDIR_BASE}/no-gh-bin"
mkdir -p "${NO_GH_BIN}"
for cmd in bash git cut rm rmdir sort grep sleep find sed mkdir date tr mktemp; do
  if [ -x "${NO_GH_BIN}/${cmd}" ]; then continue; fi
  ln -s "$(command -v "${cmd}")" "${NO_GH_BIN}/${cmd}"
done

stderr_t7="$(mktemp)"
result_t7=$(cd "${FAKE_REPO}" && PATH="${NO_GH_BIN}" bash "${SCRIPT}" --claim test-no-gh-t7 2>"${stderr_t7}") ||
  echo "note: _run exited non-zero" >&2
GIT_COMMON_T7="$(_git_common_dir)"

if [ "${result_t7}" = "0602" ]; then
  _pass "test 7a: missing gh returns expected number 0602"
else
  _fail "test 7a: expected 0602, got '${result_t7}'"
fi

if grep -q "WARNING: open pull request heads not scanned (gh not found on PATH); fork pull request ADR numbers may collide" "${stderr_t7}"; then
  _pass "test 7b: missing gh prints expected WARNING with reason 'gh not found on PATH' on stderr"
else
  _fail "test 7b: expected WARNING with 'gh not found on PATH' on stderr, got: $(cat "${stderr_t7}")"
fi

rm -f "${FAKE_REPO}/docs/adr/${result_t7}-test-no-gh-t7.md.stub"
rm -f "${GIT_COMMON_T7}/adr-claims/${result_t7}"
rm -f "${stderr_t7}"

# ── test 8: boundary: gh returns empty list (no open PRs) ────────────────────
#
# Scenario: gh succeeds and prints nothing (no open PRs).
# Expected: no WARNING on stderr, allocator returns next number from branches/master (0602).

cat >"${STUB_BIN}/gh" <<'GHEOF'
#!/usr/bin/env bash
exit 0
GHEOF
chmod +x "${STUB_BIN}/gh"

stderr_t8="$(mktemp)"
result_t8=$(_run --claim test-empty-pr-t8 2>"${stderr_t8}") || echo "note: _run exited non-zero" >&2
GIT_COMMON_T8="$(_git_common_dir)"

if [ "${result_t8}" = "0602" ]; then
  _pass "test 8a: empty PR list returns expected number 0602"
else
  _fail "test 8a: expected 0602, got '${result_t8}'"
fi

if grep -q "WARNING: open pull request heads not scanned" "${stderr_t8}"; then
  _fail "test 8b: unexpected WARNING on stderr: $(cat "${stderr_t8}")"
else
  _pass "test 8b: empty PR list prints no open-PR WARNING"
fi

rm -f "${FAKE_REPO}/docs/adr/${result_t8}-test-empty-pr-t8.md.stub"
rm -f "${GIT_COMMON_T8}/adr-claims/${result_t8}"
rm -f "${stderr_t8}"

# ── test 9: positive: gh returns 1000 SHAs (limit reached) → WARNING on stderr ─
#
# Scenario: gh stub returns exactly 1000 distinct 40-hex SHAs (limit reached).
# Expected: claim succeeds with exit 0, returns next free number,
#           and stderr contains the incomplete-scan WARNING.

cat >"${STUB_BIN}/gh" <<'GHEOF'
#!/usr/bin/env bash
for ((i = 1; i <= 1000; i++)); do
  printf '%040d\n' "${i}"
done
exit 0
GHEOF
chmod +x "${STUB_BIN}/gh"

stderr_t9="$(mktemp)"
result_t9=$(_run --claim test-limit-1000-t9 2>"${stderr_t9}") || echo "note: _run exited non-zero" >&2
GIT_COMMON_T9="$(_git_common_dir)"

if printf '%s' "${result_t9}" | grep -qE '^[0-9]{4}$'; then
  _pass "test 9a: 1000-head limit still returns 4-digit number: ${result_t9}"
else
  _fail "test 9a: 1000-head limit returned invalid output: '${result_t9}'"
fi

if grep -q "WARNING: open pull request heads scan incomplete (1000-head limit reached); fork pull request ADR numbers may collide." "${stderr_t9}"; then
  _pass "test 9b: 1000-head limit prints incomplete-scan WARNING on stderr"
else
  _fail "test 9b: expected incomplete-scan WARNING on stderr, got: $(cat "${stderr_t9}")"
fi

rm -f "${FAKE_REPO}/docs/adr/${result_t9}-test-limit-1000-t9.md.stub"
rm -f "${GIT_COMMON_T9}/adr-claims/${result_t9}"
rm -f "${stderr_t9}"

# ── test 10: negative: gh returns 999 SHAs (< limit) → no WARNING ─────────────
#
# Scenario: gh stub returns 999 distinct 40-hex SHAs (below limit).
# Expected: claim succeeds with exit 0, returns next free number,
#           and stderr does NOT contain the incomplete-scan WARNING.

cat >"${STUB_BIN}/gh" <<'GHEOF'
#!/usr/bin/env bash
for ((i = 1; i <= 999; i++)); do
  printf '%040d\n' "${i}"
done
exit 0
GHEOF
chmod +x "${STUB_BIN}/gh"

stderr_t10="$(mktemp)"
result_t10=$(_run --claim test-limit-999-t10 2>"${stderr_t10}") || echo "note: _run exited non-zero" >&2
GIT_COMMON_T10="$(_git_common_dir)"

if printf '%s' "${result_t10}" | grep -qE '^[0-9]{4}$'; then
  _pass "test 10a: 999-head scan returns 4-digit number: ${result_t10}"
else
  _fail "test 10a: 999-head scan returned invalid output: '${result_t10}'"
fi

if grep -q "WARNING: open pull request heads scan incomplete" "${stderr_t10}"; then
  _fail "test 10b: unexpected incomplete-scan WARNING on stderr: $(cat "${stderr_t10}")"
else
  _pass "test 10b: 999-head scan prints no incomplete-scan WARNING"
fi

rm -f "${FAKE_REPO}/docs/adr/${result_t10}-test-limit-999-t10.md.stub"
rm -f "${GIT_COMMON_T10}/adr-claims/${result_t10}"
rm -f "${stderr_t10}"

# ── test 11: boundary: gh prints 1 SHA + non-SHA stderr line, exit 0, TMPDIR missing ──
#
# Scenario: gh stub outputs one valid SHA on stdout and a non-SHA warning line
#   on stderr, exiting 0. TMPDIR names a directory that does not exist, so under
#   set -e any mktemp on the scan path fails the claim.
# Expected: the SHA is used, no WARNING is printed, and TMPDIR is never created.

git -C "${FAKE_REPO}" checkout --quiet -b temp-boundary-pr master
touch "${FAKE_REPO}/docs/adr/0800-boundary.md"
git -C "${FAKE_REPO}" add docs/adr/0800-boundary.md
git -C "${FAKE_REPO}" commit --quiet -m "feat: boundary pr adr 0800"
SHA_BOUNDARY="$(git -C "${FAKE_REPO}" rev-parse HEAD)"
git -C "${FAKE_REPO}" checkout --quiet master
git -C "${FAKE_REPO}" branch -D temp-boundary-pr >/dev/null
git -C "${FAKE_REPO}" update-ref "refs/pull/2/head" "${SHA_BOUNDARY}"

cat >"${STUB_BIN}/gh" <<GHEOF
#!/usr/bin/env bash
printf '%s\n' "${SHA_BOUNDARY}"
printf 'warning: new gh release available\n' >&2
exit 0
GHEOF
chmod +x "${STUB_BIN}/gh"

PRIVATE_TMPDIR="${TMPDIR_BASE}/missing-tmp-t11"

stderr_t11="$(mktemp)"
result_t11=$(TMPDIR="${PRIVATE_TMPDIR}" _run --claim test-boundary-t11 2>"${stderr_t11}") || echo "note: _run exited non-zero" >&2
GIT_COMMON_T11="$(_git_common_dir)"

if [ -n "${result_t11}" ] && [ "$((10#${result_t11}))" -gt 800 ]; then
  _pass "test 11a: boundary PR head with ADR 0800 → SHA is used, returns number above 0800: ${result_t11}"
else
  _fail "test 11a: expected number above 0800, got '${result_t11}'"
fi

if grep -q "WARNING:" "${stderr_t11}"; then
  _fail "test 11b: unexpected WARNING on stderr: $(cat "${stderr_t11}")"
else
  _pass "test 11b: non-SHA warning on stderr produces no allocator WARNING"
fi

if [ ! -e "${PRIVATE_TMPDIR}" ]; then
  _pass "test 11c: missing TMPDIR was never created"
else
  _fail "test 11c: ${PRIVATE_TMPDIR} was created: $(ls -A "${PRIVATE_TMPDIR}")"
fi

rm -f "${FAKE_REPO}/docs/adr/${result_t11}-test-boundary-t11.md.stub"
rm -f "${GIT_COMMON_T11}/adr-claims/${result_t11}"
git -C "${FAKE_REPO}" update-ref -d "refs/pull/2/head"
rm -f "${stderr_t11}"

# ── test 12: split fetch: branch ADRs survive absent PR head commit SHA ───────
#
# Scenario: an origin branch holds docs/adr/0750-remote.md.
#   gh stub returns one valid PR SHA (with docs/adr/0760-pr.md) and one absent
#   SHA not present on origin.
#   Batch PR fetch fails on the absent SHA and retries per-SHA, warning on
#   the absent SHA.
# Expected: branch tip fetch succeeds independently; allocator skips branch
#           ADR 0750 and valid PR ADR 0760, returning 0761;
#           stderr contains per-SHA warning for the absent PR head;
#           stderr does NOT report branch tips failed to fetch.

git -C "${FAKE_REPO}" checkout --quiet -b branch-t12 master
touch "${FAKE_REPO}/docs/adr/0750-remote.md"
git -C "${FAKE_REPO}" add docs/adr/0750-remote.md
git -C "${FAKE_REPO}" commit --quiet -m "feat: branch adr 0750"
SHA_BRANCH_12="$(git -C "${FAKE_REPO}" rev-parse HEAD)"

git -C "${FAKE_REPO}" checkout --quiet -b temp-pr-t12 master
touch "${FAKE_REPO}/docs/adr/0760-pr.md"
git -C "${FAKE_REPO}" add docs/adr/0760-pr.md
git -C "${FAKE_REPO}" commit --quiet -m "feat: pr adr 0760"
SHA_PR_12="$(git -C "${FAKE_REPO}" rev-parse HEAD)"

git -C "${FAKE_REPO}" checkout --quiet master
git -C "${FAKE_REPO}" branch -D branch-t12 temp-pr-t12 >/dev/null

git -C "${FAKE_REPO}" update-ref "refs/remotes/origin/branch-t12" "${SHA_BRANCH_12}"
git -C "${FAKE_REPO}" update-ref "refs/pull/12/head" "${SHA_PR_12}"

SHA_ABSENT_12="e00000000000000000000000000000000000000e"

MASTER_SHA_T12="$(git -C "${FAKE_REPO}" rev-parse master)"
cat >"${STUB_BIN}/git" <<GITEOF
#!/usr/bin/env bash
if [ "\$1" = "ls-remote" ] && [ "\$2" = "--heads" ] && [ "\$3" = "origin" ]; then
  printf '%s\trefs/heads/branch-t12\n' "${SHA_BRANCH_12}"
  printf '%s\trefs/heads/master\n' "${MASTER_SHA_T12}"
  exit 0
fi
exec "${REAL_GIT}" "\$@"
GITEOF
chmod +x "${STUB_BIN}/git"

cat >"${STUB_BIN}/gh" <<GHEOF
#!/usr/bin/env bash
printf '%s\n' "${SHA_PR_12}"
printf '%s\n' "${SHA_ABSENT_12}"
exit 0
GHEOF
chmod +x "${STUB_BIN}/gh"

stderr_t12="$(mktemp)"
result_t12=$(_run --claim test-split-fetch-t12 2>"${stderr_t12}") || echo "note: _run exited non-zero" >&2
GIT_COMMON_T12="$(_git_common_dir)"

if [ "${result_t12}" = "0761" ]; then
  _pass "test 12a: split fetch: branch ADR 0750 survived absent PR head → allocator returns 0761"
else
  _fail "test 12a: expected 0761, got '${result_t12}'"
fi

if grep -q "WARNING: could not fetch open pull request head ${SHA_ABSENT_12}; its ADR numbers are skipped." "${stderr_t12}"; then
  _pass "test 12b: per-SHA warning printed for absent PR head commit"
else
  _fail "test 12b: expected per-SHA warning on stderr, got: $(cat "${stderr_t12}")"
fi

if grep -q "could not fetch remote branch tips" "${stderr_t12}"; then
  _fail "test 12c: branch tips incorrectly reported as failed: $(cat "${stderr_t12}")"
else
  _pass "test 12c: branch tips fetch not affected by bad PR head"
fi

rm -f "${FAKE_REPO}/docs/adr/${result_t12}-test-split-fetch-t12.md.stub"
rm -f "${GIT_COMMON_T12}/adr-claims/${result_t12}"
git -C "${FAKE_REPO}" update-ref -d "refs/remotes/origin/branch-t12"
git -C "${FAKE_REPO}" update-ref -d "refs/pull/12/head"
rm -f "${stderr_t12}" "${STUB_BIN}/git"

# ── test 13: VMAF_ADR_PR_REPO environment override ───────────────────────────
#
# Scenario:
#   13a (positive): VMAF_ADR_PR_REPO is set to custom repo; stub requires -R with that value.
#   13b (negative): VMAF_ADR_PR_REPO is unset; stub requires -R with origin URL.
#   13c (boundary): VMAF_ADR_PR_REPO is empty string; falls back to origin URL.

git -C "${FAKE_REPO}" checkout --quiet -b temp-override-pr master
touch "${FAKE_REPO}/docs/adr/0770-override.md"
git -C "${FAKE_REPO}" add docs/adr/0770-override.md
git -C "${FAKE_REPO}" commit --quiet -m "feat: override pr adr 0770"
SHA_OVERRIDE="$(git -C "${FAKE_REPO}" rev-parse HEAD)"
git -C "${FAKE_REPO}" checkout --quiet master
git -C "${FAKE_REPO}" branch -D temp-override-pr >/dev/null
git -C "${FAKE_REPO}" update-ref "refs/pull/13/head" "${SHA_OVERRIDE}"

CUSTOM_REPO="VMAFx/vmafx"

# 13a: Positive case: VMAF_ADR_PR_REPO set to CUSTOM_REPO
cat >"${STUB_BIN}/gh" <<GHEOF
#!/usr/bin/env bash
has_custom_repo=0
prev=""
for arg in "\$@"; do
  if [ "\${prev}" = "-R" ] && [ "\${arg}" = "${CUSTOM_REPO}" ]; then
    has_custom_repo=1
    break
  fi
  prev="\${arg}"
done
if [ "\$1" = "pr" ] && [ "\$2" = "list" ] && [ "\${has_custom_repo}" -eq 1 ]; then
  printf '%s\n' "${SHA_OVERRIDE}"
  exit 0
fi
printf 'gh stub: expected -R %s\n' "${CUSTOM_REPO}" >&2
exit 1
GHEOF
chmod +x "${STUB_BIN}/gh"

result_t13a=$(VMAF_ADR_PR_REPO="${CUSTOM_REPO}" _run --claim test-override-pos-t13 2>/dev/null) || echo "note: _run exited non-zero" >&2
GIT_COMMON_T13="$(_git_common_dir)"

if [ "${result_t13a}" = "0771" ]; then
  _pass "test 13a: positive: VMAF_ADR_PR_REPO override passed to gh -R → allocator returns 0771"
else
  _fail "test 13a: expected 0771 with VMAF_ADR_PR_REPO override, got '${result_t13a}'"
fi
rm -f "${FAKE_REPO}/docs/adr/${result_t13a}-test-override-pos-t13.md.stub"
rm -f "${GIT_COMMON_T13}/adr-claims/${result_t13a}"

# 13b: Negative case: VMAF_ADR_PR_REPO unset → stub receives origin URL
cat >"${STUB_BIN}/gh" <<GHEOF
#!/usr/bin/env bash
has_origin_repo=0
prev=""
for arg in "\$@"; do
  if [ "\${prev}" = "-R" ] && [ "\${arg}" = "${ORIGIN_URL}" ]; then
    has_origin_repo=1
    break
  fi
  prev="\${arg}"
done
if [ "\$1" = "pr" ] && [ "\$2" = "list" ] && [ "\${has_origin_repo}" -eq 1 ]; then
  printf '%s\n' "${SHA_OVERRIDE}"
  exit 0
fi
printf 'gh stub: expected -R %s\n' "${ORIGIN_URL}" >&2
exit 1
GHEOF
chmod +x "${STUB_BIN}/gh"

result_t13b=$(
  unset VMAF_ADR_PR_REPO
  _run --claim test-override-unset-t13 2>/dev/null
) || echo "note: _run exited non-zero" >&2
if [ "${result_t13b}" = "0771" ]; then
  _pass "test 13b: negative: unset VMAF_ADR_PR_REPO falls back to origin URL → allocator returns 0771"
else
  _fail "test 13b: expected 0771 with unset VMAF_ADR_PR_REPO, got '${result_t13b}'"
fi
rm -f "${FAKE_REPO}/docs/adr/${result_t13b}-test-override-unset-t13.md.stub"
rm -f "${GIT_COMMON_T13}/adr-claims/${result_t13b}"

# 13c: Boundary case: VMAF_ADR_PR_REPO set to empty string → falls back to origin URL
result_t13c=$(VMAF_ADR_PR_REPO="" _run --claim test-override-empty-t13 2>/dev/null) || echo "note: _run exited non-zero" >&2
if [ "${result_t13c}" = "0771" ]; then
  _pass "test 13c: boundary: empty VMAF_ADR_PR_REPO falls back to origin URL → allocator returns 0771"
else
  _fail "test 13c: expected 0771 with empty VMAF_ADR_PR_REPO, got '${result_t13c}'"
fi
rm -f "${FAKE_REPO}/docs/adr/${result_t13c}-test-override-empty-t13.md.stub"
rm -f "${GIT_COMMON_T13}/adr-claims/${result_t13c}"
git -C "${FAKE_REPO}" update-ref -d "refs/pull/13/head"

# ── summary ───────────────────────────────────────────────────────────────────

printf '\n%d passed, %d failed\n' "${PASS}" "${FAIL}"
[ "${FAIL}" -eq 0 ]
