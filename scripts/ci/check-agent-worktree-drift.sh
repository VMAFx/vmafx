#!/usr/bin/env bash
# ADR-0332 — Agent worktree-drift hard guard (pre-commit).
#
# Policy: when one or more agent worktrees exist under
# `<repo-root>/.claude/worktrees/agent-*`, an active background agent is
# assumed to be running. A commit issued from the *main* checkout (i.e.
# the repo root, NOT one of the agent worktrees) is overwhelmingly
# likely to be drift — the agent's process landed in the main tree
# instead of its isolated worktree and is about to clobber the user's
# working state or commit work to the wrong branch.
#
# Detection (pre-commit time, run by .pre-commit-config.yaml):
#   1. Resolve the toplevel of the repo we're committing to via
#      `git rev-parse --show-toplevel`.
#   2. If the toplevel matches `*/.claude/worktrees/agent-*` — allow.
#      (We're inside an agent worktree; this is the intended path.)
#   3. Otherwise the toplevel is the main checkout. Look for sibling
#      agent worktrees under `<toplevel>/.claude/worktrees/agent-*`.
#      If at least one exists — REFUSE the commit with a clear error.
#      The user can bypass via the --no-verify flag (pass it to git-commit)
#      for legitimate main-checkout commits while an agent is running
#      (e.g. their own hand-edited commits to master).
#
# This guard is the host-side layer. The process-side layer is the
# global memory rule `feedback_agents_isolated_worktree_only` — agents
# should not `cd` into the main tree in the first place. Both layers
# matter; one is not a substitute for the other (see
# docs/development/agent-worktree-discipline.md).
#
# Five session-2026-05-09 incidents prompted this guard: PR #498
# (AdaptiveCpp), PR #520 (T3-15), PR #526 (ccache), MCP runtime v2
# first attempt, and the multi-corpus run. Each lost work or required
# cherry-pick recovery.
#
# References: docs/adr/0332-agent-worktree-drift-hard-guard.md.

set -euo pipefail

toplevel="$(git rev-parse --show-toplevel 2>/dev/null || true)"
if [ -z "$toplevel" ]; then
  # Not inside a git tree — pre-commit framework would not invoke us
  # here, but bail safely just in case.
  exit 0
fi

# Allow: the commit is being issued from inside an agent worktree.
case "$toplevel" in
  */.claude/worktrees/agent-*)
    exit 0
    ;;
esac

# We are in the main checkout. Look for any active agent worktree.
agent_dir="$toplevel/.claude/worktrees"
if [ ! -d "$agent_dir" ]; then
  # No agent worktree infrastructure on this clone — nothing to
  # guard against. User commits to the main checkout normally.
  exit 0
fi

# Collect agent-* directories. Empty-glob safe.
shopt -s nullglob 2>/dev/null || true
agent_worktrees=("$agent_dir"/agent-*)
if [ "${#agent_worktrees[@]}" -eq 0 ]; then
  exit 0
fi

# At least one agent worktree present. Refuse the commit.
cat >&2 <<EOF
ERROR: agent-worktree-drift guard (ADR-0332) refused this commit.

You are committing to the MAIN checkout while one or more isolated
agent worktrees are active:

EOF
# Cap the listing — clones with stale agent dirs can have dozens.
max_print=8
count=0
for wt in "${agent_worktrees[@]}"; do
  if [ "$count" -ge "$max_print" ]; then
    printf '  ... (%d more)\n' \
      "$((${#agent_worktrees[@]} - max_print))" >&2
    break
  fi
  printf '  %s\n' "$wt" >&2
  count=$((count + 1))
done
cat >&2 <<EOF

This is the drift pattern documented in
docs/development/agent-worktree-discipline.md — an agent likely
landed in the main tree instead of its assigned worktree and is
about to clobber the user's working state or commit to the wrong
branch.

Fixes (pick one):

  1. If you are an agent: \`cd\` into your assigned worktree
     (\`\$AGENT_WT\`) and re-run the commit there. Use
     \`git -C "\$AGENT_WT" commit ...\` from now on. See
     docs/development/agent-worktree-discipline.md.

  2. If you are the human user committing legitimate main-checkout
     work while a background agent runs: pass --no-verify to git-commit
     to bypass this pre-commit hook.

  3. If the listed worktree is stale (the agent finished hours ago
     and forgot to clean up), remove it with \`git worktree remove\`
     and retry.

EOF
exit 1
