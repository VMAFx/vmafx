#!/usr/bin/env bash
# scripts/adr/tests/test-next-free-remote-aware.sh
#
# Acceptance tests for the remote-aware ADR allocator (next-free.sh).
#
# Tests:
#   1. Remote branches with overlapping ADR files → allocator returns max+1.
#   2. Offline mode (mocked ls-remote failure) → local fallback + WARNING on stderr.
#   3. Sibling worktree claim via .git/adr-claims/ → second call returns higher number.
#   4. Performance smoke (must complete within 5 s on a synthetic-branch set).
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

REPO_ROOT="$(git rev-parse --show-toplevel)"
SCRIPT="${REPO_ROOT}/scripts/adr/next-free.sh"

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

# Initialise the fake repo and create a minimal origin/master state.
git -C "${FAKE_REPO}" init --quiet
git -C "${FAKE_REPO}" config user.email "test@test"
git -C "${FAKE_REPO}" config user.name "Test"
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
  gcd="${FAKE_REPO}/.git"
  lkey="$(printf '%s' "${gcd}" | tr '/' '_')"
  rmdir "/tmp/vmaf_adr_claim_lock_${lkey}" 2>/dev/null || true
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
result_t1=$(_run --claim test-remote-t1 2>/dev/null || true)
_STUBS_T1=("${FAKE_REPO}/docs/adr/${result_t1}-test-remote-t1.md.stub")
if [ "${result_t1}" = "0604" ]; then
  _pass "test 1: remote branches with overlapping ADRs → allocator returns 0604 (max+1)"
else
  _fail "test 1: expected 0604, got '${result_t1}'"
fi

# Clean up stub, branch refs, and git stub.
rm -f "${_STUBS_T1[@]+"${_STUBS_T1[@]}"}" 2>/dev/null || true
GIT_COMMON_T1="$(_git_common_dir)"
rm -f "${GIT_COMMON_T1}/adr-claims/${result_t1}" 2>/dev/null || true
# Delete the remote-tracking refs AND the local branches so that subsequent
# git ls-remote origin calls (which hit the local-path remote and return its
# refs/heads/*) do not see branch-a / branch-b.
git -C "${FAKE_REPO}" update-ref -d "refs/remotes/origin/branch-a" 2>/dev/null || true
git -C "${FAKE_REPO}" update-ref -d "refs/remotes/origin/branch-b" 2>/dev/null || true
git -C "${FAKE_REPO}" branch -D branch-a 2>/dev/null || true
git -C "${FAKE_REPO}" branch -D branch-b 2>/dev/null || true
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
result_t2=$(_run --claim test-offline-t2 2>"${stderr_t2}" || true)
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
rm -f "${FAKE_REPO}/docs/adr/${result_t2}-test-offline-t2.md.stub" 2>/dev/null || true
rm -f "${GIT_COMMON_T2}/adr-claims/${result_t2}" 2>/dev/null || true
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

result_t3=$(_run --claim test-sibling-t3 2>/dev/null || true)
if [ "${result_t3}" = "0603" ]; then
  _pass "test 3: sibling worktree claim in .git/adr-claims/ correctly blocked 0602 → returned 0603"
else
  _fail "test 3: expected 0603 (sibling claimed 0602), got '${result_t3}'"
fi

# Clean up side-pointers and stub.
rm -f "${GIT_COMMON}/adr-claims/0602"
rm -f "${GIT_COMMON}/adr-claims/${result_t3}" 2>/dev/null || true
rm -f "${FAKE_REPO}/docs/adr/${result_t3}-test-sibling-t3.md.stub" 2>/dev/null || true

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
result_t4=$(_run --claim test-perf-t4 2>/dev/null || true)
perf_end="$(date +%s%3N)"
elapsed_ms=$((perf_end - perf_start))

GIT_COMMON_T4="$(_git_common_dir)"
rm -f "${STUB_BIN}/git"
rm -f "${FAKE_REPO}/docs/adr/${result_t4}-test-perf-t4.md.stub" 2>/dev/null || true
rm -f "${GIT_COMMON_T4}/adr-claims/${result_t4}" 2>/dev/null || true

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

# ── summary ───────────────────────────────────────────────────────────────────

printf '\n%d passed, %d failed\n' "${PASS}" "${FAIL}"
[ "${FAIL}" -eq 0 ]
