#!/usr/bin/env bash
# Sanity tests for scripts/ci/check-agent-worktree-drift.sh (ADR-0332).
#
# Exercises three branches of the guard:
#   (1) commit from inside an agent worktree         → exit 0
#   (2) commit from main checkout, NO agent siblings → exit 0
#   (3) commit from main checkout WITH agent sibling → exit 1
#
# Runs entirely against tmp git repos; no network, no test data, no
# build artifacts. Invoked from CI alongside the other shellcheck /
# self-tests in scripts/ci/.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
GUARD="$SCRIPT_DIR/check-agent-worktree-drift.sh"

if [ ! -x "$GUARD" ]; then
  echo "FAIL: guard script $GUARD is not executable" >&2
  exit 2
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

mkrepo() {
  local dir="$1"
  mkdir -p "$dir"
  git -C "$dir" init -q -b master
  git -C "$dir" -c user.email=t@t -c user.name=t commit -q --allow-empty -m init
}

run_in() {
  local dir="$1"
  (cd "$dir" && "$GUARD")
}

# --- Case 1: commit from inside an agent worktree → allow ---
mkrepo "$WORK/main"
mkdir -p "$WORK/main/.claude/worktrees"
git -C "$WORK/main" worktree add -q -b feat/x \
  "$WORK/main/.claude/worktrees/agent-deadbeef"
if ! run_in "$WORK/main/.claude/worktrees/agent-deadbeef"; then
  echo "FAIL: case 1 (commit from agent worktree) was refused" >&2
  exit 1
fi

# --- Case 2: main checkout, no agent siblings → allow ---
mkrepo "$WORK/clean"
if ! run_in "$WORK/clean"; then
  echo "FAIL: case 2 (main checkout, no agents) was refused" >&2
  exit 1
fi

# --- Case 2b: main checkout, .claude/worktrees/ exists but is empty → allow ---
mkrepo "$WORK/empty"
mkdir -p "$WORK/empty/.claude/worktrees"
if ! run_in "$WORK/empty"; then
  echo "FAIL: case 2b (empty .claude/worktrees dir) was refused" >&2
  exit 1
fi

# --- Case 3: main checkout WITH active agent sibling → refuse ---
# Reuse $WORK/main from case 1 — it has agent-deadbeef alive.
if run_in "$WORK/main" 2>/dev/null; then
  echo "FAIL: case 3 (main checkout + active agent) was allowed" >&2
  exit 1
fi

echo "OK: check-agent-worktree-drift.sh passes all three cases"
