<!-- markdownlint-disable MD013 -->
# Agent cleanup preservation

The 2026-09-08 repository audit identified two independent data-loss paths in
`scripts/dev/cleanup-agent-state.sh`: dead-owner worktree removal used `--force`,
and stashes were dropped based on branch names or branch existence. Neither
decision inspected whether the state was preserved elsewhere.

The installed Git 2.55.0 `git-worktree(1)` manual states that normal removal
requires a clean checkout; `--force` permits removal with tracked changes and
untracked files. `git-stash(1)` describes stashes as commit objects referenced
through the stash reflog. It does not equate an existing branch with preservation
of the stash contents. The manual also warns that dropped stashes are outside
the normal recovery mechanisms.

Sources: [git-worktree](https://git-scm.com/docs/git-worktree) and
[git-stash](https://git-scm.com/docs/git-stash), verified using the installed
Git 2.55.0 manuals dated 2026-06-29. The implementation additionally inspects
ignored artifacts because their contents can be unique experimental evidence.

ADR-1239 selects a read-only inventory by default and exact path selection for
eligible worktree removal. Stashes are retained for separate review. This
keeps cleanup useful without asserting that abandoned work is redundant.

Run `bash scripts/dev/test-cleanup-agent-state.sh` for a reproducer independent
of repository contents. Disposable Git repositories exercise default inventory,
invalid arguments, live and unknown owners, dirty/untracked/ignored files,
unreferenced detached commits, pending Git operations, main/self protection,
whole-selection validation, restoration of a lock after failed Git removal,
pathnames containing spaces, retained branch refs and retained stashes on master,
an existing branch and detached HEAD. No project corpus or golden assertions
are involved.
