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

A selected documentation push requires MkDocs. Missing `mkdocs` blocks the
push with an installation hint; install `docs/requirements.txt` in the
active environment. Direct non-doc invocations skip before requiring the
docs toolchain. The required hosted `Docs` job installs these dependencies
and runs strict validation.
Missing `pre-commit` itself blocks framework hook dispatch with a clear
message; activate the environment used for installation.

## Post-commit private-state synchronization

The post-commit state hook resolves both the active worktree and Git's common
directory. In a linked worktree it mirrors the six canonical ledgers into a
regular ignored `.workingdir`, runs Praetor against the committing worktree,
and atomically publishes the resulting `STATE.md` to the canonical checkout.
The mirror preserves worktree-local caches and evidence; those directories are
not public documentation and are never copied back.

Synchronization is serialized by a lock in the common Git directory. Lock
contention, missing or non-regular canonical ledgers, a symlinked local state
root, and a missing synchronizer all fail the hook. A failed hook therefore
cannot silently record the main checkout's branch on behalf of an agent
worktree. See [ADR-1280](../adr/1280-worktree-state-sync.md) and its
[research digest](../research/1280-worktree-state-sync.md).

Run the helper directly from the checkout whose identity should be recorded:

```bash
scripts/githooks/state-sync.sh
```

## Python push scope

The `mypy-local` hook implements [ADR-1279](../adr/1279-mypy-fail-closed.md):
every tracked `.py` and `.pyi` path below `ai/` and `scripts/` is checked under
strict Python 3.14 semantics on every push. It does not read a merge base or
pre-commit's old-tip/new-tip file list. A one-line branch and a documentation
branch therefore run the same owned scope as hosted `Python Lint` and
`make lint-py`.

The runner partitions files by runtime import root and passes
`--explicit-package-bases`, so a file is never analysed once as a bare script
and again as an `ai.*` package. Mypy's normal import following still applies;
an owned script cannot hide an error in a repository package it imports.

### What makes the hook fail

Any mypy diagnostic blocks the push, including one in an unchanged owned file.
Missing Git, missing mypy, an empty source set, a missing tracked file, or an
internal checker failure also blocks. There is no diagnostic baseline,
merge-base subtraction, missing-import exemption, or advisory exit path.

Install the same typed environment as hosted CI:

```bash
python3 -m pip install -e './ai[dev]' -e ./tools/vmaf-tune
```

Run the same check manually from the repository root:

```bash
python3 scripts/git-hooks/pre-push-mypy.py
```

Tracked internal symlinks retain their Git filename for checking; their target
must resolve to an existing regular file inside the checkout. External,
dangling, looping, or directory targets fail. The command always derives its
own complete scope; filename arguments are rejected rather than narrowing it.

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
python3 scripts/git-hooks/test-pre-push-mypy.py
```

This runs real Git commits and pushes to disposable local repositories.
It tests installer-worktree deletion, failed commit/message/push gates,
first-push and draft documentation failures, custom-hook refusal,
framework migration, legacy hooks, native mode, and the rebase guard.
The required `Pre-Commit` CI job runs the same fixture before the normal
file checks. `make lint-sh` also runs it.

The mypy fixture creates a disposable Git repository and proves that every
tracked owned source is checked, an inherited diagnostic still blocks,
package-root invocations use explicit bases, filename arguments cannot narrow
scope, checker/setup failures block, and Make, pre-push, hosted CI, and the
strict configuration remain coupled. Its registered pre-commit/pre-push
contract check also runs in required `Pre-Commit` CI.

See [ADR-1241](../adr/1241-worktree-hook-dispatch.md) and the
[research digest](../research/1241-worktree-hook-dispatch.md).

## Disposable Git fixture safety

Git exports repository and index variables to hooks. Changing directory or
using `git -C` does not override them, so a test that creates a temporary
repository must clear inherited `GIT_*` before its first Git command and
disable caller system/global Git configuration. The FFmpeg replay/smoke,
dependency-classifier, Level Zero and agent-cleanup fixtures use this isolation
for setup and assertions as well as the operation under test.

Run their caller-preservation regression with:

```bash
python3 scripts/ci/test_git_fixture_isolation.py
```

It creates fake caller repositories with committed, staged and unstaged work,
then runs each fixture with `GIT_DIR`, `GIT_COMMON_DIR`, `GIT_WORK_TREE`,
`GIT_INDEX_FILE` and `GIT_CONFIG_PARAMETERS` individually and together.
Every fixture must succeed without changing any caller metadata or files.
A second regression invokes a real Git hook from a disposable linked worktree.
Git supplies `GIT_DIR` itself; the old unisolated `git init` control changes
the fake caller's shared `core.bare`, while the current Level Zero test must
preserve both worktrees and all shared Git metadata byte for byte. This tests
the hook environment even when the invoking shell has no Git variables.

Only temporary caller paths are injected. The local pre-commit/pre-push hook
runs when its inputs change. Required `Pre-Commit` CI also runs the regression.
