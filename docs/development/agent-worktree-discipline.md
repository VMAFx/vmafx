# Agent worktree discipline

When background coding agents (Claude Code, automation runners) work on
this repo in parallel, they each get their own isolated git worktree
under `.claude/worktrees/agent-<id>/`. Two things must hold:

1. **Process side** — agents start in, stay in, and commit from their
   assigned worktree.
2. **Host side** — `git` itself refuses commits that come from the
   *main* checkout while an agent worktree is active, because that
   pattern is overwhelmingly the agent landing in the wrong tree.

This page documents both layers. They are independent, and one is not
a substitute for the other.

## The drift pattern

"Drift" is when an agent's process ends up running with `cwd` inside
the main checkout (`/home/<user>/dev/vmaf/`) instead of its assigned
worktree (`/home/<user>/dev/vmaf/.claude/worktrees/agent-<id>/`).
The agent then `git commit`s into the main checkout's HEAD, which is
usually `master` or whatever branch the human user has checked out.
Common consequences:

- The user's uncommitted local edits to the main checkout are
  clobbered or, worse, mixed into a commit that ends up under the
  agent's PR.
- The agent's commit lands on the wrong branch (e.g. `master`) and
  has to be cherry-picked off and force-rolled-back, which can race
  with branch protection.
- Two agents collide on the same files in the main tree.

Five drift incidents in the 2026-05-09 session prompted the host-side
guard described below: PR #498 (AdaptiveCpp), PR #520 (T3-15),
PR #526 (ccache), the first attempt at the MCP runtime v2 PR, and
the multi-corpus run. Each lost work or required cherry-pick recovery.

## Layer 1 — process-side discipline

Per global memory `feedback_agents_isolated_worktree_only`: never
spawn parallel agents in the shared tree. Use
`isolation: "worktree"` on the agent task, or pre-create a worktree
with `git worktree add` and pass that path as the agent's cwd.

The canonical pattern an agent should follow:

```bash
# At session start — refuse to do anything if cwd drifted.
pwd | grep -q '\.claude/worktrees/' || {
    echo "DRIFT: cwd is not inside an agent worktree" >&2
    exit 1
}

AGENT_WT="$(pwd)"

# Use absolute paths and `git -C "$AGENT_WT"` for every git call.
git -C "$AGENT_WT" status
git -C "$AGENT_WT" add path/to/file
git -C "$AGENT_WT" commit -m "..."
```

Agent-side rules of thumb:

- Resolve every path relative to `$AGENT_WT`, not via `cd` + relative
  paths. The shell state can reset between bash calls in some
  harnesses; `cd` does not survive.
- Verify `git rev-parse --show-toplevel` equals `$AGENT_WT` every
  ~20 tool uses. Stop and ask the user if it doesn't.
- Never `cd` into the main checkout. If the agent needs to read a
  file there for inspection, use absolute paths and `Read`/`grep`
  rather than `cd`.

## Layer 2 — host-side hard guard (ADR-0332)

The script `scripts/ci/check-agent-worktree-drift.sh` runs as a
pre-commit hook (wired through `.pre-commit-config.yaml` and
installed by `make hooks-install`). It refuses any commit that:

- Originates from the main checkout (`git rev-parse --show-toplevel`
  matches the repo root), AND
- Has at least one sibling agent worktree active under
  `<repo-root>/.claude/worktrees/agent-*`.

That conjunction matters. The guard does not refuse main-checkout
commits when no agent worktree is alive — the user's own commits to
master from the main checkout pass freely. It only fires when an
agent could plausibly be the source of the commit.

### Sample blocked-commit error

```text
ERROR: agent-worktree-drift guard (ADR-0332) refused this commit.

You are committing to the MAIN checkout while one or more isolated
agent worktrees are active:

  /home/user/dev/vmaf/.claude/worktrees/agent-deadbeef
  /home/user/dev/vmaf/.claude/worktrees/agent-cafef00d
  ... (3 more)

This is the drift pattern documented in
docs/development/agent-worktree-discipline.md — an agent likely
landed in the main tree instead of its assigned worktree and is
about to clobber the user's working state or commit to the wrong
branch.
```

### Bypassing the guard

The guard honours git's standard escape hatch: `git commit
--no-verify`. Use it when:

- You are the human user committing your own legitimate work to the
  main checkout while a background agent is running.
- You are doing emergency cleanup (e.g. a `git revert` that has to
  land on master immediately and the agent worktrees are stale but
  not yet pruned).
- Tooling (release-please, automated bots) commits non-interactively
  with a known-good cwd.

Do **not** bypass when you are an agent and the guard fired against
your commit. That is the exact pattern the guard exists to catch.
`cd` to your worktree (or use `git -C "$AGENT_WT"`) and re-run.

### Installing the hook

```bash
make hooks-install
```

`make hooks-install` runs `pre-commit install --install-hooks`,
which writes the `agent-worktree-drift-guard` `local` hook from
`.pre-commit-config.yaml` into `.git/hooks/pre-commit`. Idempotent:
re-running just refreshes the bound script.

### Testing the guard

```bash
bash scripts/ci/test_check_agent_worktree_drift.sh
```

Three cases — commit-from-agent-WT (allow), main-WT-no-siblings
(allow), main-WT-with-active-agent (refuse) — all run against
disposable temp git repos. No build artifacts, no network.

## Why two layers and not just one

Layer 1 alone (agent-side discipline) is fragile: agents drift
because their harness's shell state resets, or because a `cd` did not
survive, or because the harness silently retried in a fresh process
group. Telling the agent "be more careful" doesn't scale.

Layer 2 alone (host-side guard) leaves a window: between the agent
issuing the wrong `git add` and the pre-commit hook firing, the
agent has already staged files in the main checkout's index. The
hook prevents the commit, but the staged state can still confuse
the human. Process-side discipline avoids the staging in the first
place.

Both layers run together. The guard is the safety net that catches
the cases the agent missed.
