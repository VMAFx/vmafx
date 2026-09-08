# Git fixture initialization inside linked-worktree hooks

The Level Zero configuration test created a temporary repository with
`git init -q <temporary path>`, but inherited the process environment. A real
Git hook invoked from a linked worktree exports `GIT_DIR` identifying that
worktree's Git administration directory. This happens even when the invoking
shell has no repository variables.

A disposable-repository reproduction invoked the exact old initialization
command through `git hook run pre-commit`. Git returned success and changed
the fake caller's shared `core.bare` from false to true. An absolute temporary
path and `git -C` do not establish repository isolation. This confirms the
mechanism; it does not retrospectively identify the writer of every observed
configuration change in a shared checkout.

The implementation follows the existing fixture isolation invariant: clear
all inherited `GIT_*`, disable system/global configuration, and pass that
explicit environment to both initialization and the checker subprocess.
There is no alternative policy decision or new ADR: this repairs a missing
consumer of the existing isolation contract.

Reproduce the old-command control and verify the fixed helper:

```bash
python3 scripts/ci/test_git_fixture_isolation.py \
  GitFixtureIsolation.test_real_linked_worktree_hook_preserves_shared_repository
python3 scripts/ci/test_git_fixture_isolation.py
```

The first command creates independent disposable main/linked pairs with
committed, staged and unstaged work. It compares all caller and linked files,
including shared Git configuration, objects, refs, worktree administration and
both indexes. The negative control must change its fake caller's bare setting;
the actual current Level Zero test must complete without changing any caller
byte. The wider matrix covers five Git variables individually and together
for all five fixture families. It uses no real caller Git paths or network.

This is tooling validation, not native numerical, backend, release, or full
repository acceptance. Netflix golden assertions are untouched.
