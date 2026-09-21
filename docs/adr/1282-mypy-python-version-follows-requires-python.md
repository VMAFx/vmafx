<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1282: mypy's `python_version` follows `requires-python`

- **Status**: Proposed
- **Date**: 2026-09-21
- **Deciders**: Lusoris
- **Tags**: ci, python, tooling, hooks, ai, fork-local

## Context

`pyproject.toml` declared `requires-python = ">=3.14"` and every CI leg installs
`python-version: "3.14.7"`, but `[tool.mypy]` pinned `python_version = "3.10"`.
The pin was carried forward from before the project raised its floor and was
tracked as `T-CI-MYPY-PYTHON-VERSION-STALE-2026-09-19`.

Modelling an unsupported language version is not merely inaccurate here — it
stops the check. mypy refuses to parse a PEP 695 `type` statement when told to
target anything below 3.12, and numpy's bundled `__init__.pyi` contains one at
line 737. In any checkout that has numpy installed (which is every checkout that
can run the AI tree, and the local pre-push hook's environment), that produced a
blocking `[syntax]` diagnostic and mypy stopped: the `ai/src/` pass reported
`Found 40 errors in 22 files (errors prevented further checking)` and checked
**zero** source files. At 3.14 the same pass completes —
`Found 112 errors in 26 files (checked 40 source files)`. Sixty findings across
the `ai/` and `scripts/` trees were invisible purely because the type checker had
been aborted before semantic analysis.

ADR-1261 deferred this raise to its own change on the basis that it "unmasks 175
findings on master". Measured again here: none of those findings are *created* by
the raise. Checking this branch against a copy of its merge base with the same
`3.14` value applied gives a delta of **0** introduced findings. The debt is
pre-existing and was being hidden, which is the strongest argument for raising
the pin rather than a reason to defer it further.

## Decision

`[tool.mypy] python_version` tracks `requires-python`, and is therefore `"3.14"`.
It moves in lockstep whenever the project's floor moves. The findings the raise
makes visible are pre-existing debt and are left standing, not suppressed: no
`type: ignore`, no `ignore_missing_imports` widening and no gate relaxation lands
with this change.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Raise to `3.14` (chosen) | Models the only version the project supports; numpy's stubs parse, so `ai/src/` is actually analysed for the first time | Makes 60 pre-existing findings visible; the pre-push delta gate reports them once during the transition | — |
| Delete `python_version` entirely | One less value to keep in sync | mypy then floats with whatever interpreter the caller happens to run, so contributors and CI could model different versions from the same tree | Determinism is worth the one line |
| Keep `3.10` until the findings are paid down | No transition noise | Leaves the checker aborting on numpy and the `ai/src/` pass checking nothing; the debt stays invisible and keeps growing | The pin is what hid the debt; paying it down first requires being able to see it |
| Raise, and add the missing third-party modules to `ignore_missing_imports` to flatten the count | A smaller reported number | Suppression, not a fix; those findings vary with the checkout's installed stubs by design (ADR-1261) | Forbidden by the fork's no-suppression rule |

## Consequences

- **Positive, and only where numpy is installed**: in a checkout that has the
  AI stack's dependencies — every developer checkout that can run `ai/`, and the
  environment the local `pre-push-mypy.py` hook runs in — mypy now parses
  numpy's stubs and completes the `ai/src/` pass instead of aborting, so the
  strict settings this file already declares actually apply to that tree.
  `python_version`, `requires-python` and `PYTHON_CI_VERSION` agree, and
  `check_mypy_python_version` in `scripts/ci/check-workflow-versions.py` now
  fails the always-run pre-commit gate if they ever stop agreeing — including if
  `python_version` is deleted rather than reverted. Fixture:
  `scripts/ci/tests/test_mypy_python_version_single_source.py`.
- **No change at all to CI's mypy job.** The `Python Lint` job
  (`.github/workflows/lint-and-format.yml`) installs only `mypy` and runs
  `mypy ai/ scripts/ || echo "mypy advisory only on first run"`. numpy is not
  installed there, so its stubs are never parsed and the PEP 695 abort this ADR
  is about never happened in CI. Measured in a venv holding nothing but mypy
  2.3.1, that command's combined output is **byte-identical** at `3.10` and at
  `3.14` — 392 lines, md5 `14d59427cd97323723510b0d14100ca2`, ending
  `Found 382 errors in 171 files (errors prevented further checking)` in both.
  That job checked zero source files before this change and still checks zero
  after it, for an unrelated reason: with no dependencies installed every import
  is unresolvable (325 `import-not-found`, 56 `import-untyped`) and
  `ai/src/vmaf_train/models/__init__.py` is additionally found twice under two
  module names, which aborts the run before semantic analysis. The `|| echo`
  means it is advisory regardless. Making that job check anything is a separate
  problem — it needs the AI stack installed or `--explicit-package-bases`, and
  neither belongs in this change.
- **Negative**: the pre-push delta gate recomputes its baseline from the *merge
  base's* `pyproject.toml`. For the single change that moves this value, the
  baseline is still evaluated at `3.10` while the branch is evaluated at `3.14`,
  so every newly-visible finding is attributed to the branch — 103 over this
  branch's 55 changed Python files. Measured against a merge base carrying the
  same `3.14`, the delta is 0. Once the value is on `master` the asymmetry is
  gone for every subsequent branch.
- **Neutral / follow-ups**:
  - The 60 newly-visible findings (20 `untyped-decorator`, 11 `misc`
    subclass-of-`Any`, 9 `type-arg`, 8 `no-any-return`, 7 `has-type`, plus
    single `redundant-cast`, `unused-ignore`, `assignment` and two
    `no-untyped-def`) are their own clean-up. Most are consequences of
    torch / lightning / pydantic not being installed in the checking
    environment and so vary with it, exactly as ADR-1261 documents.
  - A gate that re-evaluates its baseline under the merge base's own
    configuration is self-blocking for any change to that configuration.
    Teaching `scripts/git-hooks/pre-push-mypy.py` to apply the branch's
    `[tool.mypy]` block to the baseline worktree belongs to
    `T-CI-MYPY-PREPUSH-BASELINE-USES-BASE-CONFIG-2026-09-21`, not here. (The
    similarly-named `T-CI-MYPY-PREPUSH-BLOCKS-ON-INHERITED-2026-09-19` is a
    different, already-closed finding about which *findings* the gate
    attributes, not which *configuration* its baseline resolves.)
  - `[tool.black] target-version` (`py310`/`py311`/`py312`) and
    `[tool.ruff] target-version` (`py310`) carry the same staleness. They are
    formatter and linter rule selectors rather than a type model, and moving
    them rewrites code across the tree, so they are left to a separate change.

## References

- `T-CI-MYPY-PYTHON-VERSION-STALE-2026-09-19` and
  `T-CI-MYPY-PREPUSH-BASELINE-USES-BASE-CONFIG-2026-09-21` in
  [docs/state.md](../state.md).
- [ADR-1261](1261-mypy-pre-push-delta-gate.md) — the delta gate that deferred
  this raise to its own change.
- [PEP 695](https://peps.python.org/pep-0695/) — the `type` statement numpy's
  `__init__.pyi` uses, which mypy rejects below `python_version = 3.12`.
- [mypy configuration reference](https://mypy.readthedocs.io/en/stable/config_file.html#confval-python_version)
  — `python_version` selects the language version the checker models.
