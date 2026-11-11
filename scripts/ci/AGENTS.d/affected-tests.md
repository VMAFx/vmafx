---
paths:
  - scripts/ci/run_affected_suites.py
  - scripts/ci/tests/test_run_affected_suites.py
  - scripts/lib/push_head.py
  - .pre-commit-config.yaml
invariant: Focused push tests reuse registry; clean selected HEAD paths, unmapped-file discovery, NOT RUN reasons + CI coverage.
area: gates
---
# Affected push tests

- Runner: `run_affected_suites.py`; suite registry, locks, venv cache reused.
- `--focused`: changed tests; exact path/module tokens; unique tracked basenames;
  unique Python stems in static/literal dynamic imports; package imports ->
  `__init__.py`, uniqueness over package directories. Base tree + index paths
  for deleted-name uniqueness. Pure selector; no second mapping.
- Compat alias: `/python/vmaf/__init__.py` -> `/compat/vmaf` symlink ->
  `/compat/python-vmaf`; `vmaf` imports cover package + shim.
- Outside every suite trigger: search every locally runnable suite's tests;
  exact matches add suite + matching files. Registry triggers unchanged.
- Unmatched source, including unmapped paths: `NOT RUN <path>`, reason
  `no test names this file`, then `alternate coverage: pull request CI jobs: ...`.
- Empty suite: `NOT RUN (no test names a changed file)`; zero passed tests;
  alternate coverage line. `not_local`: reason + alternate coverage line.
- Coverage names: registry `checks`; Tooling Tests workflow:
  `/.github/workflows/tests-and-quality-gates.yml`, `suite_registry.py run tooling`.
- `--strict`: unrun suite or unmatched path fails; default recorded skip = 0.
  Test failure = 1; usage error, unreadable file, dirty checkout = 2.
- After selection, before execution: changed paths + selected tests = HEAD
  in index AND worktree; else dirty paths named, exit 2. Unrelated tracked
  edits/untracked files outside selected paths irrelevant. `--list`: selection only.
- Installed pre-commit: ordinary pre-push stashes unstaged changes only;
  first-push `all_files=True` bypasses stash; staged edits remain visible.
- Known gap: assembled basename, e.g. `"check-codeql-" + "unused-static.py"`,
  no complete path/module token -> NOT selected; full CI suite coverage remains.
- `--merge-base REF`: merge-base to HEAD; exclusive with base/head/files.
- Shared `scripts/lib/push_head.py`: outgoing ref != checkout HEAD -> refuse;
  all `GIT_` environment removed from ref-resolution subprocess.
- Hook `affected-tests`: always, serial, pre-push; first venv build, ai ~6 GB.
- Regression: `tests/test_run_affected_suites.py`; mypy consumer:
  `/scripts/git-hooks/test-pre-push-mypy.py`.
