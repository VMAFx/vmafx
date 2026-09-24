# Research-2092: Pre-push type-check baseline evaluates branch checker configuration

## Scope and problem statement

State item `T-CI-MYPY-PREPUSH-BASELINE-USES-BASE-CONFIG-2026-09-21` identified
a self-blocking defect in the pre-push type-check delta gate
(`scripts/git-hooks/pre-push-mypy.py`, introduced in ADR-1261):

```text
The pre-push type-check delta gate is self-blocking for any change to its own configuration.
scripts/git-hooks/pre-push-mypy.py re-checks the branch's files at the merge base in a
disposable worktree, and that worktree carries the merge base's pyproject.toml. When a
branch edits [tool.mypy], the two sides of the comparison are therefore evaluated under
different settings, and every finding the new setting makes visible is attributed to
the branch.
```

Measured on `fix/bug-mypy-pyver`, which changed `python_version` from 3.10 to
3.14 (ADR-1282): the hook reported 103 introduced findings across inherited
Python files on the branch against 0 when the merge base was evaluated under
the same 3.14 setting.

## Root cause analysis

The delta gate operates by comparing mypy findings on the branch head against
findings on the merge base:

1. `selected_paths()` determined which files to analyze. Previously, it only
   checked `git diff --name-only base...head` for files ending in `.py`. If a
   branch edited only configuration (e.g. `pyproject.toml`), `selected_paths()`
   returned an empty set, bypassing validation entirely.
2. For branches touching both source and configuration, the disposable baseline
   worktree (`git worktree add --detach <tmp> <base>`) checked out the repository
   at `base`. The baseline execution ran using `base`'s checker configuration
   files (`pyproject.toml`, `mypy.ini`, etc.).
3. When `head` introduced stricter type checking, new flags, or higher language
   versions, existing debt in the baseline files went unflagged in the baseline
   run, but was reported on `head`. The delta calculation
   `new_fingerprints = head_fingerprints - base_fingerprints` attributed every
   newly unmasked finding to the branch as an introduced finding, blocking push.

## Self-block reproduction

The self-block was reproduced in `scripts/git-hooks/test-pre-push-mypy.py` via
`test_branch_only_mypy_config_reproduces_self_block_without_copied_config`.
When a branch adds a stricter configuration rule (e.g. `strict = true` in
`[tool.mypy]`), an unchanged source file containing pre-existing debt triggers
a finding under the new setting. Without copying branch configuration into the
baseline worktree:

- Baseline worktree (running under `base` config) reports 0 findings.
- Branch worktree (running under `head` config) reports 1 finding.
- The hook computes `introduced = 1` and exits with status 1 (blocked).

When branch configuration is synchronized to the baseline worktree:

- Baseline worktree reports 1 inherited finding.
- Branch worktree reports 1 finding.
- The hook computes `introduced = 0` and permits the push.

## Module-identity blocker exposed by the widened scope

The first implementation widened a checker-configuration change to every tracked
Python file under `ai/` and `scripts/`, then failed before it could compare
findings:

```text
ai/src/vmaf_train/__init__.py: error: Source file found twice under different
module names: "vmaf_train" and "ai.src.vmaf_train"
```

`ai/src` is the configured `mypy_path` and therefore owns the canonical
`vmaf_train.*` identity. Four compatibility call sites still import the same
tree through the non-runtime `ai.src.vmaf_train.*` alias. A full-scope run saw
both import graphs at once. `exclude = ["ai/src/"]` only affects recursive file
discovery, and `ignore_errors = true` is applied after discovery, so neither can
prevent mypy from rejecting the duplicate module identity.

The `ai.src.*` per-module override now uses `follow_imports = "skip"`. Mypy does
not traverse that compatibility alias, while the canonical `vmaf_train.*` tree
continues to be checked by the dedicated `ai/src` invocation with
`--explicit-package-bases`. This resolves each source file once without editing
the overlapping AI-helper restoration branch.

## Implementation details

The fix in `scripts/git-hooks/pre-push-mypy.py` implements the following
mechanisms:

1. **Configuration file tracking & extraction**:
   Monitors canonical configuration locations: `pyproject.toml`, `mypy.ini`,
   `.mypy.ini`, `setup.cfg`.
   Extracts `[tool.mypy]` via `tomllib` and `[mypy*]` sections via
   `configparser`. Changes to unrelated sections in `pyproject.toml`
   (e.g. `[tool.black]`) are ignored.

2. **Scope widening on configuration changes**:
   When `mypy_config_changed(root, base, head)` detects an altered checker
   configuration, `selected_paths()` expands selection to all tracked `.py`
   files under the supported check roots (`ai/` and `scripts/`). If
   configuration is unchanged, diff-only file selection is preserved.

3. **Baseline configuration synchronization**:
   Before executing the baseline checker in the temporary worktree:
   - Any configuration file present in the branch is copied into the baseline
     worktree.
   - Any configuration file deleted in the branch is removed (`unlink`) from
     the baseline worktree.
   - All source code files at the merge base remain untouched.

4. **Preservation of merge-base source files**:
   Verified by
   `test_branch_mypy_config_change_preserves_merge_base_source_files`. If a
   branch modifies configuration and also introduces a syntax or type error in
   a source file, the baseline worktree evaluates the original merge-base
   source under the new configuration, correctly identifying the source change
   as an introduced finding.

5. **Fail-closed semantics**:
   - Syntax errors in configuration files (e.g. invalid TOML) raise errors and
     terminate execution with exit code 2.
   - Mypy exit 0 is success and exit 1 is the ordinary findings status. Any
     other status is a blocking analysis error and terminates the hook with exit
     2, even if mypy printed partial parseable findings before aborting. Exit 1
     without an attributable finding is also rejected.

6. **Canonical module identity**:
   The `pyproject.toml` override for the legacy `ai.src.*` namespace skips
   following that alias. Canonical `vmaf_train.*` modules remain checked in the
   `ai/src` explicit-package-base pass.

## Verification evidence

The regression suite in `scripts/git-hooks/test-pre-push-mypy.py` verifies all
aspects:

- `test_branch_only_mypy_config_change_evaluates_baseline_under_branch_config`:
  PASS
- `test_branch_only_mypy_config_reproduces_self_block_without_copied_config`:
  PASS (proves red-to-green transition)
- `test_branch_mypy_config_change_preserves_merge_base_source_files`: PASS
- `test_unrelated_pyproject_change_does_not_trigger_full_check`: PASS
- `test_mypy_config_fails_closed_on_invalid_toml`: PASS
- `test_baseline_checker_failure_fails_closed`: PASS
- `test_checker_blocker_with_a_finding_fails_closed`: PASS
- `test_baseline_blocker_with_a_finding_fails_closed`: PASS
- `test_earlier_exit_1_does_not_mask_later_blocker_across_mypy_groups`:
  PASS
- `test_earlier_blocker_does_not_get_masked_by_later_exit_1_across_mypy_groups`:
  PASS
- `test_baseline_earlier_exit_1_does_not_mask_later_blocker_across_mypy_groups`:
  PASS
- `test_branch_deleted_mypy_config_unlinks_baseline_config`: PASS
- `test_config_change_resolves_ai_source_root_once`: PASS (real mypy; reproduces
  the exact duplicate-module abort without the alias traversal guard)

Full test run:

```text
python3 scripts/git-hooks/test-pre-push-mypy.py
Ran 32 tests
OK
```

Formatting and lint checks (`black --check`, `ruff check`) pass cleanly.
