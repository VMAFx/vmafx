#!/usr/bin/env bash
# scripts/ci/test-state-md-touch-check.sh — fixtures for state-md-touch-check.sh.
#
# Stand-alone bash-only test driver. Builds a throw-away git repo
# under $(mktemp -d) so the diff input is real (not mocked), then
# exercises five canned cases covering the gate's predicate space:
#
#   1. fix: prefix in title, no state.md, no opt-out → FAIL
#   2. fix: prefix in title, with 'no state delta: REASON' → PASS
#   3. fix: prefix in title, diff touches state.md → PASS
#   4. neutral title, body has UNCHECKED Bug-status row → FAIL
#   5. neutral title, diff is docs-only, no trigger → PASS (gate inert)
#
# Plus a trio of regression cases that previously confused the
# trigger heuristic ("debug" should not fire, "Closes #" must fire,
# "BUG" upper-case must fire).
#
# Run from anywhere:
#   bash scripts/ci/test-state-md-touch-check.sh
#
# Prints a one-line summary per case and a final PASS / FAIL count.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GATE="${SCRIPT_DIR}/state-md-touch-check.sh"

if [ ! -x "$GATE" ]; then
  echo "test-state-md-touch-check: gate script not executable: $GATE" >&2
  exit 2
fi

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

# Build a tiny git repo we can run real diffs against. We don't
# need a real master tip — just two commits so BASE_SHA..HEAD_SHA
# resolves.
(
  cd "$WORKDIR"
  git init -q -b master
  git config user.email "test@example.com"
  git config user.name "Test"
  mkdir -p docs
  printf 'seed\n' >README.md
  git add README.md
  git commit -q -m "seed"
)

REPO="$WORKDIR"
BASE_SHA="$(git -C "$REPO" rev-parse HEAD)"

# Helpers ---------------------------------------------------------------

# make_commit <commit-msg> <relpath> <content>
# Stages a single file and commits, leaves HEAD on a new commit.
make_commit() {
  local msg="$1" rel="$2" content="$3"
  mkdir -p "$REPO/$(dirname "$rel")"
  printf '%s\n' "$content" >"$REPO/$rel"
  git -C "$REPO" add "$rel"
  git -C "$REPO" commit -q -m "$msg"
}

# Run gate against the current HEAD with the given title + body.
# Echoes "PASS" or "FAIL <exit-code>" — never aborts the harness.
run_gate() {
  local title="$1" body="$2"
  local head_sha
  head_sha="$(git -C "$REPO" rev-parse HEAD)"
  local rc=0
  (
    cd "$REPO"
    PR_TITLE="$title" \
      PR_BODY="$body" \
      BASE_SHA="$BASE_SHA" \
      HEAD_SHA="$head_sha" \
      bash "$GATE"
  ) >/tmp/state-md-gate.out 2>&1 || rc=$?
  if [ "$rc" -eq 0 ]; then
    echo "PASS"
  else
    echo "FAIL($rc)"
  fi
}

# reset_repo — drop HEAD back to BASE_SHA (idempotent between cases).
reset_repo() {
  git -C "$REPO" reset -q --hard "$BASE_SHA"
}

# Case runner -----------------------------------------------------------

ok=0
ng=0
total=0

# expect <case-name> <wanted: PASS|FAIL> <gate-output: PASS|FAIL(N)>
expect() {
  local name="$1" want="$2" got="$3"
  total=$((total + 1))
  case "$got" in
    "$want" | "$want"*)
      printf '  [OK]  %-60s want=%s got=%s\n' "$name" "$want" "$got"
      ok=$((ok + 1))
      ;;
    *)
      printf '  [NG]  %-60s want=%s got=%s\n' "$name" "$want" "$got"
      ng=$((ng + 1))
      echo "      --- gate output ---"
      sed 's/^/      /' /tmp/state-md-gate.out
      ;;
  esac
}

echo "test-state-md-touch-check: running fixtures…"

# Case 1: fix: prefix, no state.md edit, no opt-out → FAIL ----------
reset_repo
make_commit "fix: NPD-1080p fallback" "core/src/feature/feature_x.c" "fix"
got=$(run_gate "fix: NPD-1080p fallback in YUV geometry parser" "Body without opt-out.")
expect "1. fix: title + no state.md + no opt-out" "FAIL" "$got"

# Case 2: fix: prefix, body has 'no state delta: REASON' → PASS ------
reset_repo
make_commit "fix: cosmetic" "core/src/feature/feature_x.c" "fix"
got=$(run_gate "fix: cosmetic typo in log message" \
  "## Summary

Trivial log-message typo fix.

no state delta: pure cosmetic, no bug-status impact.")
expect "2. fix: title + opt-out 'no state delta: ...'" "PASS" "$got"

# Case 3: fix: prefix, diff touches docs/state.md → PASS ------------
reset_repo
make_commit "fix: real fix" "core/src/feature/feature_x.c" "fix"
make_commit "docs(state): record fix" "docs/state.md" "Recently closed row"
got=$(run_gate "fix: real fix that closes a bug" "Body with no opt-out.")
expect "3. fix: title + docs/state.md in diff" "PASS" "$got"

# Case 4: neutral title, UNCHECKED Bug-status checkbox → FAIL --------
reset_repo
make_commit "feat: refactor X" "core/src/feature/feature_x.c" "ref"
got=$(run_gate "feat: refactor feature X" \
  "## Bug-status hygiene (ADR-0165)

- [ ] [\`docs/state.md\`](../docs/state.md) updated in this PR with a row in the appropriate section.
")
expect "4. UNCHECKED Bug-status row in body" "FAIL" "$got"

# Case 5: neutral title, docs-only diff, no trigger → PASS (inert) ---
reset_repo
make_commit "docs: refresh build doc" "docs/development/build-flags.md" "refresh"
got=$(run_gate "docs: refresh build-flags documentation" \
  "## Summary

Pure docs refresh, no code changes.")
expect "5. neutral title + docs-only diff + no trigger" "PASS" "$got"

# Regression: 'debug' in title must NOT fire ----------------------
reset_repo
make_commit "feat: debug-mode flag" "core/src/feature/feature_x.c" "ref"
got=$(run_gate "feat: add --debug-mode CLI flag" "Plain body, no opt-out.")
expect "R1. 'debug' substring must not trigger" "PASS" "$got"

# Regression: 'Closes #123' in body must fire even with neutral title -
reset_repo
make_commit "feat: tweak" "core/src/feature/feature_x.c" "ref"
got=$(run_gate "feat: small tweak" \
  "## Summary

Closes #123.")
expect "R2. 'Closes #123' in body triggers gate" "FAIL" "$got"

# Regression: 'BUG-1234' in title (caps) must fire -----------------
reset_repo
make_commit "feat: tweak" "core/src/feature/feature_x.c" "ref"
got=$(run_gate "BUG-1234 fix the foo" "Body without opt-out.")
expect "R3. 'BUG-' uppercase token triggers gate" "FAIL" "$got"

# ---------- Placeholder-ref hardening (ADR-0334 status update 2026-05-09) ----
#
# PR #541's audit surfaced that "touched state.md" is necessary but
# not sufficient: closing PRs were writing "this PR" as the closer
# placeholder and never rewriting it post-merge. The hardening
# rejects placeholder forms in inserted lines.
#
# Helper: stage docs/state.md with the given line, run gate.
set_state_md_and_run() {
  local row="$1" title="$2" body="$3"
  reset_repo
  mkdir -p "$REPO/docs"
  printf '# state\n\n%s\n' "$row" >"$REPO/docs/state.md"
  git -C "$REPO" add docs/state.md
  git -C "$REPO" commit -q -m "docs(state): add row"
  run_gate "$title" "$body"
}

# Case P1: state.md inserted line with "closed by this PR" → REJECT --------
got=$(set_state_md_and_run \
  "| foo | closed by this PR | bug-fix |" \
  "fix: foo segfault" \
  "Body without opt-out.")
expect "P1. inserted 'closed by this PR' rejected" "FAIL" "$got"

# Case P2: state.md inserted line with merged numeric PR ref → ACCEPT -------
got=$(set_state_md_and_run \
  "| foo | closed by PR #432 | bug-fix |" \
  "fix: foo segfault" \
  "Body without opt-out.")
expect "P2. inserted 'closed by PR #432' accepted" "PASS" "$got"

# Case P3: state.md inserted line with commit SHA ref → ACCEPT --------------
got=$(set_state_md_and_run \
  "| foo | closed by commit \`f809ce09\` | bug-fix |" \
  "fix: foo segfault" \
  "Body without opt-out.")
expect "P3. inserted 'closed by commit f809ce09' accepted" "PASS" "$got"

# Case P4: lowercase 'this pr' variant → REJECT (case-insensitive) ----------
got=$(set_state_md_and_run \
  "| foo | this pr (fix/foo, 2026-05-09) | bug-fix |" \
  "fix: foo cosmetic" \
  "Body without opt-out.")
expect "P4. inserted 'this pr' (lowercase) rejected" "FAIL" "$got"

# Case P5: 'this commit' placeholder → REJECT --------------------------------
got=$(set_state_md_and_run \
  "| foo | this commit | bug-fix |" \
  "fix: foo cosmetic" \
  "Body without opt-out.")
expect "P5. inserted 'this commit' rejected" "FAIL" "$got"

# Case P6: bare 'TBD' as closer ref → REJECT --------------------------------
got=$(set_state_md_and_run \
  "| foo | TBD | bug-fix |" \
  "fix: foo cosmetic" \
  "Body without opt-out.")
expect "P6. inserted bare 'TBD' rejected" "FAIL" "$got"

# Case P7: '<PR>' template placeholder → REJECT -----------------------------
got=$(set_state_md_and_run \
  "| foo | closed by <PR> | bug-fix |" \
  "fix: foo cosmetic" \
  "Body without opt-out.")
expect "P7. inserted '<PR>' template placeholder rejected" "FAIL" "$got"

# Case P8: '#NNN' literal placeholder → REJECT ------------------------------
got=$(set_state_md_and_run \
  "| foo | closed by PR #NNN | bug-fix |" \
  "fix: foo cosmetic" \
  "Body without opt-out.")
expect "P8. inserted '#NNN' template placeholder rejected" "FAIL" "$got"

# Case P9: regression — must NOT match 'this PR' inside a *removed* line.
# Build a state.md with the placeholder, then delete that line in a new
# commit. The placeholder appears in the diff as a `-` line (removed),
# which the gate must not flag.
reset_repo
mkdir -p "$REPO/docs"
printf '# state\n\n| old | this PR | x |\n' >"$REPO/docs/state.md"
git -C "$REPO" add docs/state.md
git -C "$REPO" commit -q -m "docs(state): seed with placeholder"
# Delete the placeholder row (still touches state.md so the touch
# predicate passes); the only diff is a removal, no insertions match.
printf '# state\n\n' >"$REPO/docs/state.md"
git -C "$REPO" add docs/state.md
git -C "$REPO" commit -q -m "docs(state): drop stale row"
got=$(run_gate "fix: drop stale row" "Body without opt-out.")
expect "P9. removed-line 'this PR' is NOT flagged" "PASS" "$got"

# Case P10: 'debug-pr' substring inside a real word must NOT match -----
got=$(set_state_md_and_run \
  "| foo | closed by PR #432 (debug-pr branch) | bug-fix |" \
  "fix: foo cosmetic" \
  "Body without opt-out.")
expect "P10. 'debug-pr' substring (no whitespace) does not match 'this PR'" "PASS" "$got"

echo ""
echo "test-state-md-touch-check: ${ok}/${total} passed, ${ng} failed."

if [ "$ng" -ne 0 ]; then
  exit 1
fi
