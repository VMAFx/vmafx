# Research-2071: Repository subprocess boundary audit

## Scope

The audit covered Python repository automation under `scripts/ci`,
`scripts/dev`, `scripts/docs`, `scripts/git-hooks`, `scripts/githooks`, and
`scripts/lib`. It followed the executable paths as well as their disposable
Git, analyzer, backend-probe, FFmpeg-patch, hook, and workflow tests.

## Findings

- Sixty-three direct process launches were removed from 38 Python modules.
- `S603` annotations documented that a command was believed safe but imposed
  no runtime constraint if a later edit changed its executable or arguments.
- Many calls had no deadline. A stuck analyzer, Git subprocess, encoder probe,
  or hook fixture could consume the parent job's full timeout.
- Captured child output generally had no combined memory ceiling.
- A timeout could stop the immediate process while leaving descendants alive.
- Cancelling an in-flight asynchronous call propagated `CancelledError`
  without terminating or reaping the child process group.
- Call sites implemented executable discovery, Git-environment isolation,
  decoding, and failure propagation inconsistently.
- CI impact routing did not classify 19 already-tracked top-level entries, so
  an edit there fell back to full mode without making the routing debt visible.

## Resolution

`scripts/lib/safe_subprocess.py` now validates the executable against an
explicit allowlist, resolves it before launch, rejects malformed arguments and
environments plus oversized arguments, bounds captured output, closes unused
stdin, and enforces a wall-clock deadline. On POSIX, each child owns a new
session and timeout, output-overflow, or caller-cancellation cleanup terminates
the process group and reaps the direct child. The API has synchronous and
asynchronous forms so an async caller cannot accidentally block its active
event loop.

The migration keeps each caller's prior success/failure contract, replaces
direct launch annotations with runtime checks, and preserves test injection
seams. The CI-impact map now classifies every tracked top-level entry.

The initial pre-push run found a package-identity defect: mypy saw the helper
as both `lib.safe_subprocess` and `scripts.lib.safe_subprocess` when it checked
the helper beside a consumer. `scripts/__init__.py`, canonical imports, and a
two-root regression test now make that identity unique. Text and binary result
subclasses preserve stream types, while uncaptured streams return typed empty
values. The blocking check now excludes site packages and missing-import
diagnostics so an arbitrary installed PEP 561 package cannot make the gate fail
before reporting a repository finding. Clearing the stricter type surface also
removed 23 pre-existing mypy findings in the migrated files; the final
48-file isolated invocation reports none.

## Failure controls

The helper tests exercise binary and text output, replacement environments,
non-zero status preservation, closed and supplied stdin, rejected executable
and argument values, output flooding, timeouts, descendant cleanup, caller
cancellation, and the case where a session leader exits before its child.
Agent-eligibility CLI regressions also prove that GitHub command failures stay
fail-soft with the canonical exception identity. Consumer tests drive real
Git, Node, Python, hook, analyzer, and patch-stack fixtures.

## Evidence

```text
python3 -m pytest -q <all migrated test modules>
263 passed, 1 skipped, 246 subtests passed

.venv/bin/python scripts/git-hooks/test-pre-push-mypy.py
Ran 19 tests ... OK

.venv/bin/python -m unittest scripts.lib.test_safe_subprocess \
  scripts.ci.tests.test_agent_eligibility_precheck
Ran 13 tests ... OK

.venv/bin/mypy --no-site-packages \
  --disable-error-code=import-not-found <48 changed Python files>
Success: no issues found in 48 source files

python3 -m unittest scripts/ci/tests/test_scorecard_gate.py \
  scripts/ci/tests/test_scorecard_workflow.py
Ran 29 tests ... OK

praetorctl audit
HISS invariant scan verified: 1399 active violations within 1411 baselined
limit (40 touched files clean)

ruff check <changed Python files>
All checks passed!
```

The active repository still carries historical HISS and application-package
process-execution debt outside this automation slice. Those counts are
migration inventory, not acceptance waivers; subsequent batches must remove
them rather than expanding or citing the baseline as clean.
