<!-- markdownlint-disable MD013 -->

# ADR-1261: The local type-check hook fails on findings a branch introduces, not on ones it inherits

- **Status**: Proposed
- **Date**: 2026-09-19
- **Deciders**: Lusoris
- **Tags**: ci, hooks, python, tooling, fork-local

## Context

`mypy` runs in two places, with two different contracts.

In CI, the `Python Lint` job runs `mypy ai/ scripts/ || echo "mypy advisory only on first run"`. The `||` is deliberate and the job comment gives the reason: stub coverage for numpy, pandas and torch is uneven. The job is therefore advisory and has never blocked a merge.

Locally, `scripts/git-hooks/pre-push-mypy.py` runs the same checker over the Python files a branch changed, and its exit status blocks the push. That made the local hook stricter than the gate it mirrors, and the difference was not a matter of degree:

- **Nothing under `ai/src/` could be pushed at all.** `ai/src` is on mypy's `mypy_path`, so a file under it has two possible module names, and mypy refuses outright with "Source file found twice under different module names" when such a path is named on the command line. The `exclude` entry in `pyproject.toml` only prevents discovery while crawling. Measured on 2026-09-19: a branch that edited `ai/src/aiutils/run_manifest.py` could not be pushed, and neither could `master` itself have been, had it been a branch.
- **Inherited findings blocked unrelated work.** Whether a finding appears depends on which of numpy, pandas and torch the checkout has installed, so a developer environment richer than CI's reports more. Measured on the same day, in a checkout with those packages present: the 23 files a dependency-restoration branch touched produced 14 findings, and `master`'s own copies of the same files produced the same 14. None belonged to the branch.

Raising `python_version` from its stale `3.10` to the `3.14` the project actually requires unmasks 175 further findings on `master`'s AI tree. That is real debt worth paying down, but it is not something a branch can be asked to carry as the price of pushing.

## Decision

The hook stays blocking, and it measures a delta.

1. Files under `ai/src/` are checked in their own invocation with `--explicit-package-bases`, which names them from the `mypy_path` base alone, matching the module they have at runtime. Files outside it keep the plain invocation, so their module names, and the per-module overrides keyed on them, do not change.
2. The same file set is checked again at the branch's merge base, in a disposable worktree, and only findings absent there are reported. Line numbers are not part of a finding's identity, so an edit above a finding does not make it look new.
3. A non-zero exit with no attributable finding fails the push. That is mypy breaking rather than a clean run, and it must not pass silently.

`python_version` and the 175 findings behind it are left to their own change.

## Alternatives considered

| Option | Why not |
| --- | --- |
| **Delta gate (chosen)** | Keeps the hook meaningful: a branch may add no type error. Costs one extra checker run and a disposable worktree per push. |
| Make the hook advisory, like CI | Matches the CI contract exactly, and gives up the only place where mypy actually gates anything. A gate that cannot fail is documentation. |
| Fix the 175 inherited findings first, keep the hook absolute | The right end state, and far too large to sit in front of every unrelated branch. It is also not stable: the finding set moves with the local environment's installed packages. |
| Exclude `ai/src/` from the hook's file selection | Removes the hard error without checking those files at all, which is a coverage hole, not a fix: `ai/src` holds the shared AI helpers. |
| Raise `python_version` to 3.14 in the same change | Measured: 175 findings on `master`'s AI tree, and a second class of error from `compat/python-vmaf` not being an importable package name. A separate change with its own risk. |

## Consequences

- A branch that edits a file under `ai/src/` can be pushed.
- A branch that adds a type error is still rejected, and the message names the finding and says that inherited ones are not listed.
- Each push runs the checker twice and creates one disposable worktree under the cache directory, removed even when the run fails, so the ADR-0332 worktree-drift guard sees nothing.
- The inherited-finding count is printed, so the debt stays visible instead of silently accepted.
- Paying the debt down needs no change here: as findings disappear from the merge base, they disappear from the baseline.

## References

- `.github/workflows/lint-and-format.yml`, the `Python Lint` job: `mypy ai/ scripts/ || echo "mypy advisory only on first run"`.
- `scripts/git-hooks/pre-push-mypy.py` and its regression suite `scripts/git-hooks/test-pre-push-mypy.py`.
- [ADR-0922](0922-coverage-ratchet-aggressive.md) — the same delta shape for coverage, which this follows.
- [ADR-0332](0332-agent-worktree-drift-hard-guard.md) — why the baseline worktree must always be removed.
- `req` (paraphrased, standing instruction): a pre-existing defect is not a reason to leave a gate broken; fix it.
