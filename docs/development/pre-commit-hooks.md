# Local Git hooks

Run `make install-hooks` from the checkout or linked worktree you use.
The installer requires Python with `pre-commit` installed in the active
environment and prepares the configured hook environments before replacing
any hooks. `make hooks-install` remains an alias. Install `pre-commit` from
the hash lock, which also carries `reuse` for the `reuse-lint` check:

```bash
python3 -m pip install --require-hashes -r requirements/locks/pre-commit.txt
make install-hooks
```

Lefthook owns three hooks next to these dispatchers; install it first, as
described in [Lefthook and the governance hooks](#lefthook-and-the-governance-hooks).

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
twin drift, mypy, bounded-process regressions, FFmpeg patch replay, PR
deliverables, and MkDocs strict validation. The PR-body check may skip a first
push or draft PR;
that does not skip the other checks. Existing framework `.legacy` hooks
continue to run in framework stages.

A selected documentation push requires MkDocs. Missing `mkdocs` blocks the
push with an installation hint; install `docs/requirements.txt` in the
active environment. Direct non-doc invocations skip before requiring the
docs toolchain. The required hosted `Docs` job installs these dependencies
and runs strict validation.
Missing `pre-commit` itself blocks framework hook dispatch with a clear
message; activate the environment used for installation.

## Lefthook and the governance hooks

[`lefthook.yml`](../../lefthook.yml) owns `pre-commit`, `pre-push` and
`post-commit` ([ADR-1249](../adr/1249-praetor-governance-adoption.md)). Its
`pre-commit` and `pre-push` jobs run the pre-commit framework stage through
`scripts/git-hooks/framework-hooks.sh`, next to the praetor governance checks
(`compile-context --verify`, `audit`, `hiss coverage --verify`, `dedupe scan`).
`post-commit` synchronises private state. The dispatchers keep `commit-msg`
(Conventional Commits) and `pre-rebase` (worktree drift guard). Install
lefthook first, then the dispatchers:

```bash
lefthook install
make install-hooks   # reports "left lefthook's .../pre-commit in place"
```

`make install-hooks` leaves the hooks lefthook wrote in place
([ADR-1385](../adr/1385-lefthook-installer-coexistence.md)). In the opposite
order it stops at the first hook it did not write, for example Git LFS's
`pre-push`.

The governance checks need `praetorctl` at the engine commit pinned as
`PRAETOR_REF` in `.github/workflows/standards-gate.yml`. A newer engine can
fail an unchanged tree, so put the pinned build first on `PATH`:

```bash
ref=$(sed -n 's/^  PRAETOR_REF: //p' .github/workflows/standards-gate.yml)
GOBIN="$HOME/go/bin-pinned" go install "github.com/cordanaLLM/praetor/cmd/standardsctl@$ref"
cp "$HOME/go/bin-pinned/standardsctl" "$HOME/go/bin-pinned/praetorctl"  # add .exe on Windows
export PATH="$HOME/go/bin-pinned:$PATH"
```

Two lefthook behaviours affect every checkout that shares the Git directory:

- `lefthook run <hook>` installs the hook shims first unless you pass
  `--no-auto-install`. Linked worktrees share one hooks directory, so a trial
  run in a worktree installs lefthook for the main checkout too.
- `lefthook uninstall` rewrites `.claude/settings.json` and `.codex/hooks.json`
  with sorted keys even when it removes nothing. Both files are committed in
  that form, so the rewrite leaves them unchanged; keep them that way
  (`json.dumps(indent=2, sort_keys=True)` plus a newline).
  `scripts/githooks/tests/test_install.py` checks it.

Keep every `run:` value in `lefthook.yml` on one line and free of double quotes.
On Windows lefthook passes it to `sh -c` without escaping, so a double quote
ends the script early; put logic in a script under `scripts/git-hooks/`
instead. The same test rejects both.

## Windows hosts

The hooks run in Git for Windows' Bash. Set up a checkout once:

1. Clone with `core.autocrlf=false`
   (`git clone -c core.autocrlf=false ...`). Several gates compare generated
   files byte for byte against their LF form, and Git for Windows' default of
   `true` checks them out with CRLF.
2. Create a Python 3.14 virtualenv from the hash lock:

   ```bash
   py -3.14 -m venv .venv
   .venv/Scripts/python.exe -m pip install --require-hashes -r requirements/locks/pre-commit.txt
   ```

   `framework-hooks.sh` prefers `.venv/Scripts/pre-commit.exe` (or
   `.venv/bin/pre-commit` elsewhere) over `pre-commit` on `PATH` and puts the
   virtualenv first on `PATH` for the checks, so `reuse` resolves without
   activating it. The lock installs `reuse` with `charset-normalizer`, because
   reuse cannot use `python-magic` on Windows.
3. Put lefthook v2 on `PATH`
   (`go install github.com/evilmartians/lefthook/v2@v2.1.14`) and the pinned
   `praetorctl` first on `PATH`, as above.
4. Install from a shell with the virtualenv active
   (`source .venv/Scripts/activate`): `lefthook install`, then
   `bash scripts/githooks/install.sh` (the command behind `make install-hooks`).
   The `commit-msg` dispatcher looks `pre-commit` up on `PATH` when it runs, so
   commit from a shell with the virtualenv active, or install the same lock into
   the Python on `PATH`. Lefthook's jobs find `.venv` themselves.
5. For pushes, install the push-stage tools into the same virtualenv:
   `requirements/locks/mypy.txt` (with `--require-hashes`) and
   `docs/requirements.txt` for the MkDocs strict build. `govulncheck` on
   `PATH` enables the `security` job; without it the job reports a skip.

Check the result with `lefthook run pre-commit --no-auto-install` on a staged
change. A few fixture tests assume POSIX tools and skip on Windows with the
reason printed; the Linux CI jobs still run them. Others still fail on Windows
and run only when `Makefile`, `.pre-commit-config.yaml` or their own files
change; see `T-HOOKS-WINDOWS-POSIX-FIXTURES-2026-09-30` in
[docs/state.md](../state.md) and
[Research-1385](../research/1385-lefthook-windows-host.md).

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

The `mypy-local` hook implements the touched-file rule in
[`AGENTS.md` §12.10](../../AGENTS.md): every added, copied, modified,
renamed or type-changed `*.py` path under `ai/` and `scripts/` in
`git diff origin/master...HEAD` is checked. Deleted paths are omitted.
The strict settings in `pyproject.toml` still apply. Fetch `origin/master`
before validating a rebased branch; a missing merge base blocks the push.

The hook runs once on every push and derives this complete set itself.
Pre-commit's old-remote-tip/new-tip file list can include unrelated changes
from master and omit an unchanged branch-owned file whose imports changed
after a rebase. Neither omission may narrow the check. This does not expand
the explicit file policy to other Python packages or unchanged master files.
Mypy's normal import checking still applies to the selected sources.

### What makes the hook fail

The hook reports only findings the branch introduces
([ADR-1261](../adr/1261-mypy-pre-push-delta-gate.md)). It checks the selected
files, then checks the same files again as they are at the merge base in a
disposable worktree, and lists what is new. Line numbers are left out of the
comparison, so inserting a line above an existing finding does not make it
look new. The output ends with a count of the findings that were inherited and
therefore not reported.

Two consequences worth knowing:

- A file that already fails still fails for its own reasons, and you may edit
  it. Adding a *different* finding to it is reported; the ones that were
  already there are not.
- A non-zero exit with nothing to attribute to a file is treated as mypy
  breaking, and blocks the push.

The delta keeps inherited debt from blocking unrelated changes. The required
hosted `Python Lint` job and the local hook both run this same gate; neither
uses a separate whole-tree exception list. Pull requests compare with the
fetched `origin/master`, while a master push supplies the event's exact prior
commit through `VMAFX_MYPY_BASE_REF`, so post-merge CI checks the Python paths
that actually landed rather than comparing `HEAD` with itself.

Both blocking callers pass `--no-site-packages` and
`--disable-error-code=import-not-found`. Its result therefore does not depend
on arbitrary PEP 561 packages in the active environment. Selected repository
sources, repository imports that resolve, and standard-library types are still
checked. The hosted job installs the same hash-locked mypy toolchain from
`requirements/locks/mypy.txt`; it does not install or run the training stack.
This isolation prevents a newer ambient stub from changing the gate or crashing
it before any attributable finding is emitted.

Files under `ai/src/` are checked in a separate run with
`--explicit-package-bases`. That directory is a `mypy_path` base, so without
the flag mypy sees each file under two module names and refuses the run
outright.

Run the same check manually from the repository root:

```bash
python3 scripts/git-hooks/pre-push-mypy.py
```

CI may select an explicit comparison authority without changing the local
default:

```bash
VMAFX_MYPY_BASE_REF=<previous-commit> \
  python3 scripts/git-hooks/pre-push-mypy.py
```

The checked-out HEAD must match the outgoing commit supplied by pre-commit;
push a different branch from its own checkout. Missing Git, mypy or selected
files blocks validation. Internal symlinks retain their Git filename for
selection and checking; their target must resolve to an existing regular file
inside the checkout. External, dangling, looping or directory targets fail.
The command always derives its own scope; filename arguments do not narrow it.

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
python3 -m unittest scripts.lib.test_safe_subprocess \
  scripts.ci.tests.test_agent_eligibility_precheck
```

This runs real Git commits and pushes to disposable local repositories.
It tests installer-worktree deletion, failed commit/message/push gates,
first-push and draft documentation failures, custom-hook refusal,
framework migration, legacy hooks, native mode, and the rebase guard.
The required `Pre-Commit` CI job runs the same fixture before the normal
file checks. `make lint-sh` also runs it.

The mypy fixture exercises a real rebase and the installed pre-commit
framework, including an empty outgoing file list, type changes, safe and
unsafe symlinks, ref mismatches and missing prerequisites. Its registered
pre-commit/pre-push check also runs in required `Pre-Commit` CI.

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
