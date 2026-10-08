## Test fixtures never write to the hook's repository (2026-10-09)

- `scripts/lib/clean-git-env.sh` is the one shell implementation of the isolation: it drops every
  `GIT_*` variable, ignores global and system git configuration and supplies the fixture identity
  through `GIT_AUTHOR_*` / `GIT_COMMITTER_*`. Test scripts source it before their first git command;
  none runs `git config user.*`. **On sync**: a script added upstream or by a branch that builds a
  scratch repository gets the same line; `scripts/ci/test_git_fixture_isolation.py` names the ones
  that do not (identity scan and sentinel run).
- No rebase impact on Netflix code: every file is fork-only test tooling.
