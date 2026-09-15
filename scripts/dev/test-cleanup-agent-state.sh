#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
# Copyright 2026 Lusoris
# Copyright 2026 Claude (Anthropic)
# Regression tests for ADR-1239; all mutations use disposable repositories.

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
SCRIPT="$SCRIPT_DIR/cleanup-agent-state.sh"
WORK=$(mktemp -d)
trap 'rm -rf -- "$WORK"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
REPO="$WORK/main"
mkdir -p "$REPO"
git -C "$REPO" init -q -b master
git -C "$REPO" config user.name 'Cleanup Test'
git -C "$REPO" config user.email 'cleanup-test@example.invalid'
git -C "$REPO" config core.hooksPath /dev/null
printf 'base\n' >"$REPO/tracked"
printf 'cache/\n' >"$REPO/.gitignore"
git -C "$REPO" add tracked .gitignore
git -C "$REPO" commit -qm 'test: initial fixture'

# A reaped process supplies a real, syntactically valid dead PID.
sleep 0.01 &
DEAD_PID=$!
wait "$DEAD_PID"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}
run() { (cd "$REPO" && bash "$SCRIPT" "$@"); }
reject() {
  if run "$@" >"$WORK/rejected.log" 2>&1; then
    fail "unsafe invocation succeeded: $*"
  fi
}
add_worktree() {
  local name="$1"
  git -C "$REPO" worktree add -q -b "${name// /-}" "$WORK/$name"
  git -C "$REPO" worktree lock --reason "test owner (pid $DEAD_PID)" "$WORK/$name"
}

for name in 'agent-clean space' agent-other agent-dirty agent-untracked agent-ignored agent-live agent-unknown agent-detached agent-operation non-agent; do
  add_worktree "$name"
done
printf 'unique edit\n' >>"$WORK/agent-dirty/tracked"
printf 'unique evidence\n' >"$WORK/agent-untracked/report.txt"
mkdir "$WORK/agent-ignored/cache"
printf 'ignored evidence\n' >"$WORK/agent-ignored/cache/report.txt"
git -C "$REPO" worktree unlock "$WORK/agent-live"
git -C "$REPO" worktree lock --reason "test owner (pid $$)" "$WORK/agent-live"
git -C "$REPO" worktree unlock "$WORK/agent-unknown"
git -C "$REPO" worktree lock --reason 'portable disk; owner unknown' "$WORK/agent-unknown"
git -C "$WORK/agent-detached" switch -q --detach
printf 'detached-only commit\n' >>"$WORK/agent-detached/tracked"
git -C "$WORK/agent-detached" add tracked
git -C "$WORK/agent-detached" commit -qm 'test: preserve detached commit'

touch "$(git -C "$WORK/agent-operation" rev-parse --git-path index.lock)"

# Stashes on master, a still-existing branch and detached HEAD contain unique work.
printf 'stash on master\n' >>"$REPO/tracked"
git -C "$REPO" stash push -qm 'unique master work'
git -C "$REPO" switch -qc stash-branch
printf 'stash on branch\n' >>"$REPO/tracked"
git -C "$REPO" stash push -qm 'unique branch work'
git -C "$REPO" switch -q --detach
printf 'stash on detached HEAD\n' >>"$REPO/tracked"
git -C "$REPO" stash push -qm 'unique detached work'
git -C "$REPO" switch -q master
git -C "$REPO" stash list --format='%H %gd %s' >"$WORK/stashes.before"
git -C "$REPO" worktree list --porcelain >"$WORK/worktrees.before"

run >"$WORK/default.log"
run --dry-run >"$WORK/dry-run.log"
git -C "$REPO" worktree list --porcelain >"$WORK/worktrees.after"
cmp "$WORK/worktrees.before" "$WORK/worktrees.after"
reject --apply
reject --unknown-option
reject --dry-run --apply
reject --apply --worktree relative-path
reject --apply --worktree "$WORK/missing"
reject --apply --worktree "$REPO"
reject --apply --worktree "$WORK/agent-clean space" --worktree "$WORK/agent-clean space"
for name in agent-dirty agent-untracked agent-ignored agent-live agent-unknown agent-detached agent-operation non-agent; do
  reject --apply --worktree "$WORK/$name"
  [[ -d "$WORK/$name" ]] || fail "removed protected $name"
done
reject --apply --worktree "$WORK/agent-clean space" --worktree "$WORK/agent-dirty"
[[ -d "$WORK/agent-clean space" ]] || fail 'partially applied invalid selection'
if (cd "$WORK/agent-other" && bash "$SCRIPT" --apply --worktree "$WORK/agent-other") >"$WORK/self.log" 2>&1; then
  fail 'removed the current worktree'
fi

# Failed Git removal must restore the pre-existing owner lock.
REAL_GIT=$(command -v git)
mkdir "$WORK/bin"
cat >"$WORK/bin/git" <<'WRAPPER'
#!/usr/bin/env bash
if [[ "${3:-}" == worktree && "${4:-}" == remove ]]; then exit 70; fi
exec "$REAL_GIT" "$@"
WRAPPER
chmod +x "$WORK/bin/git"
if (
  export REAL_GIT
  PATH="$WORK/bin:$PATH" run --apply --worktree "$WORK/agent-other"
) >"$WORK/failed-remove.log" 2>&1; then
  fail 'accepted a failed Git removal'
fi
git -C "$REPO" worktree list --porcelain >"$WORK/failed-remove-state"
cmp "$WORK/worktrees.before" "$WORK/failed-remove-state"

# Explicitly selected, clean checkout is retired; other checkout and branch survive.
run --apply --worktree "$WORK/agent-clean space" >"$WORK/applied.log"
[[ ! -e "$WORK/agent-clean space" ]] || fail 'selected checkout remains'
[[ -d "$WORK/agent-other" ]] || fail 'unselected checkout was removed'
git -C "$REPO" show-ref --verify --quiet refs/heads/agent-clean-space
git -C "$REPO" stash list --format='%H %gd %s' >"$WORK/stashes.after"
cmp "$WORK/stashes.before" "$WORK/stashes.after"
[[ "$(cat "$WORK/agent-untracked/report.txt")" == 'unique evidence' ]] || fail 'untracked evidence changed'
[[ "$(cat "$WORK/agent-ignored/cache/report.txt")" == 'ignored evidence' ]] || fail 'ignored evidence changed'
git -C "$REPO" fsck --connectivity-only --no-dangling --no-progress
printf 'PASS: inventory, argument validation, target isolation, work preservation, lock restoration and stash retention\n'
