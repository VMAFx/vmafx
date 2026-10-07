---
paths:
  - scripts/ci/suite_registry.py
  - .pre-commit-config.yaml
  - scripts/ci/tests/test_suite_registry.py
  - .github/test-suites.json
  - requirements/locks/tooling-tests.in
invariant: Every test file is in one suite with required checks; Tooling Tests runs registry, not hand list.
---
<!-- markdownlint-disable MD013 MD060 -->
# Test-suite registry (ADR-1528)

`.github/test-suites.json` maps every tracked test file (`name_patterns`
and `path_patterns`) to exactly one suite and every suite to required
checks that run it. `suite_registry.py check` runs first in `Tooling Tests`
job and as `suite-registry` pre-commit hook.

- new test directory goes into suite's `paths`, or into new suite whose
  check is in aggregator's `required` list. Do not add it to `not_tests` to
  make check pass: `not_tests` is for files that are not tests (drivers,
  dataset modules), each with reason.
- `run tooling` builds its file list from registry. Do not replace it with
  hand-written list of files or directories in workflow, which is how
  60 script tests went unrun.
- check fails on suite path or `not_tests` entry that matches no file, so
  rename updates registry in same commit.
- `_run_pytest` runs pytest in-process: `safe_subprocess` executes
  resolved interpreter, which would leave virtual environment that holds
  `requirements/locks/tooling-tests.txt` (pytest-timeout would vanish).
- tooling lock carries docs stack, semgrep, reuse and pre-commit
  because tests of suite import or drive them. Without them docs tests
  skip, and `test_semgrep_vendored_scope.py` and `scripts/githooks/tests` fail.

- `Python Package Tests (vmaf-tune)` is its own job with `needs: [mcp-smoke]`:
  it runs MCP Smoke's `vmaf` (artifact `vmaf-cli-mcp`, tar that keeps
  executable bit and `$ORIGIN/../src` SONAME chain) and fails on skip for
  missing binary or missing golden YUVs. It runs files
  `suite_registry.py list vmaf-tune` prints, never directory, so file
  another suite owns does not run twice. predictor trainer and its
  tests live in `ai/` since ADR-1886 (suite `ai`, `Tiny AI`); never add torch
  to vmaf-tune (`scripts/ci/check-torch-scope.py`). aggregator's
  `delayedStrictDependencies` must keep `MCP Smoke` entry for it, or
  check reads as never reported while MCP Smoke builds.

- test runs once in CI (ADR-1568). Do not add workflow step that runs
  file of tooling suite: `check` refuses it. test that needs tool only
  one job installs gets its own suite naming that job; most specific
  registry path owns file (`helm-chart`, `ffmpeg-patches`). contract test
  asserts `suite_members(ROOT, "tooling")`, not workflow step.
- CI Pre-Commit job skips `precommit-skip`'s hooks (entries that run only
  tooling tests) plus `ffmpeg-input-contract`, which FFmpeg Patch Stack runs.
  A hook whose entry also runs check keeps running there.

Tests: `scripts/ci/tests/test_suite_registry.py` (unwired file, file in two
suites, non-required check, stale entries, duplicate keys, failing Python
and shell tests, suite runner cannot run, and repository itself).
