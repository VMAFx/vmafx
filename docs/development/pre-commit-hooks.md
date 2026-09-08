# Local Git hooks

Run `make install-hooks` from the checkout or linked worktree you use.
The installer requires Python with `pre-commit` installed in the active
environment and prepares the configured hook environments before replacing
any hooks. `make hooks-install` remains an alias.

```bash
python3 -m pip install pre-commit
make install-hooks
```

The installer writes regular dispatcher files to Git's effective hooks
directory, including a configured `core.hooksPath`. Each invocation finds
its current worktree with Git. Removing the worktree used for installation
therefore cannot break hooks in the surviving checkout.

## Installed checks

| Git event | Framework mode, the default | Native mode |
| --- | --- | --- |
| `pre-commit` | Configured formatters and checks | Three native formatters |
| `commit-msg` | Configured Conventional Commits validation | Same framework check |
| `pre-push` | All configured push checks | Same framework checks |
| `pre-rebase` | Agent worktree drift guard | Same guard |

The dispatcher forwards Git's arguments and pushed-ref input to
`pre-commit hook-impl`. The framework selects checks and changed files
from `.pre-commit-config.yaml`. Push checks include assertion density,
twin drift, mypy, FFmpeg patch replay, PR deliverables, and MkDocs strict
validation. The PR-body check may skip a first push or draft PR;
that does not skip the other checks. Existing framework `.legacy` hooks
continue to run in framework stages.

MkDocs remains optional locally: missing `mkdocs` prints an installation
hint. Install `docs/requirements.txt` to enable it. The required hosted
`Docs` job installs these dependencies and runs strict validation.
Missing `pre-commit` itself blocks framework hook dispatch with a clear
message; activate the environment used for installation.

## Existing hooks and migration

The installer recognizes its own dispatchers, unmodified framework
hooks, and the repository's historical source symlinks, including links
into deleted `.claude/worktrees/` directories. It retains replaced
managed hooks in uniquely named `HOOK.vmafx-backup-*` files. Repeating an
unchanged installation makes no new backups.

Unknown regular hooks and symlinks cause installation to stop before
changing any hook. Review the reported paths and relocate or integrate
your custom hook explicitly before retrying. Existing backups are never
overwritten. Merely fetching the fix does not repair an already dangling
symlink: rerun `make install-hooks` from a checkout containing the fix.

Dispatch uses each active worktree's checked-out configuration. Installing
from an updated worktree repairs hook lifetime everywhere, but an older
branch does not acquire newly registered checks until its source config
is updated. In particular, older configs still lack the MkDocs push entry.

For inspection without installing:

```bash
git config --show-origin --get core.hooksPath
git rev-parse --path-format=absolute --git-path hooks
```

## Native formatting option

```bash
VMAFX_NATIVE_HOOKS=1 make install-hooks  # opt in
make install-hooks                      # restore the default
```

Native mode preserves the formatter-only choice in
[ADR-0924](../adr/0924-native-pre-commit-hooks.md). It runs installed
`ruff check --fix`, `clang-format -i`, and `shfmt -w` on matching staged
paths and restages files changed by formatters. Missing formatters print
a notice. Its pre-commit stage does not run the framework's security,
metadata, or agent-drift checks; use the default for those local checks.
Commit-message validation and push checks still use the framework in
both modes. CI uses the full framework configuration.

The native formatter reads working-tree files and restages the whole file
when it changes one. Use the framework path for partially staged files to
preserve the unstaged portion through the framework's stash/restore flow.

## Regression checks

```bash
python3 scripts/githooks/tests/test_install.py
```

This runs real Git commits and pushes to disposable local repositories.
It tests installer-worktree deletion, failed commit/message/push gates,
first-push and draft documentation failures, custom-hook refusal,
framework migration, legacy hooks, native mode, and the rebase guard.
The required `Pre-Commit` CI job runs the same fixture before the normal
file checks. `make lint-sh` also runs it.

See [ADR-1241](../adr/1241-worktree-hook-dispatch.md) and the
[research digest](../research/1241-worktree-hook-dispatch.md).
