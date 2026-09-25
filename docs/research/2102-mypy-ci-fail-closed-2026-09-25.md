<!-- markdownlint-disable MD013 MD060 -->
# Research-2102: required mypy gate restoration — 2026-09-25

**Status:** Complete

**Authorities inspected:** original collector base
`13aad630a5dced4618230b78d29f773812a73adf` and rebased integration collector
`8236bc821445ec2125a84038f181c6c0c31a8f8b`

**Scope:** `.github/workflows/lint-and-format.yml`, the existing pre-push mypy
gate, its contract tests, and the seven newly exposed collector findings. No
training, dependency-version, generated-model, or Netflix golden change.

## Parent reproduction

A Python 3.14 virtual environment installed only the reviewed
`requirements/locks/mypy.txt` lock. The workflow command itself produced:

```text
raw_status=1
Found 1834 errors in 276 files (checked 370 source files)
mypy advisory only on first run
workflow_status=0
```

The current tree therefore no longer matches the 2026-09-21 ledger's “zero
files checked” measurement: later module-discovery configuration lets mypy
reach hundreds of sources. The live defect is stronger and simpler: the
command reports errors, then `|| echo` turns failure into a green required job.
After rebasing onto collector `8236bc821`, the same reproduction grew with the
integrated sidecar work but retained the same fail-open result:

```text
raw_status=1
Found 1840 errors in 278 files (checked 373 source files)
workflow_status=0
```

Running the existing canonical split in the same tool-only environment removed
the directory-identity blocker and ambient import-not-found flood, while still
performing semantic analysis:

```text
Found 1345 errors in 221 files (checked 370 source files)
```

The merge-base gate reduced that inherited inventory to four findings actually
introduced between `origin/master` and the collector:

- three annotations in `ai/scripts/eval_probabilistic_proxy.py` (bare
  `dict`/`list` generics and an untyped smoke-corpus return);
- one untyped injected callback in `ai/tests/test_bvi_dvc_dir_mode.py`.

Adding precise annotations closes those four findings; the same command then
reports no new findings while retaining the inherited count.

## Rebased collector reconciliation

Collector `8236bc821` added three parameterized online-trainer tests after the
original audit. The required gate correctly rejected all three because
`pytest.mark.parametrize` is untyped in the checker-only environment and erased
the otherwise-complete test signatures. A typed `_parametrize()` adapter,
matching the existing test pattern elsewhere under `ai/tests/`, preserves each
decorated signature without a suppression. The exact collector parent reports
all seven findings; the rebased candidate reports zero introduced findings.

## Prior-work audit

| Candidate | Finding | Reuse decision |
|---|---|---|
| `agent-bug-mypy-pyver` | Correctly opened this ledger row, but its old measurement predates current module-discovery repairs | Keep the historical diagnosis, replace stale current-state claims |
| `mypy-fail-closed` | Prototype installs the complete AI stack, changes pre-push semantics, and touches about 280 Python files | Reject wholesale; too broad and stale |
| `mypy-hiss-aux` | Worktree contains hundreds of staged/unstaged Python edits on top of unrelated HISS work | Reject; not a clean reusable head |
| `agent-mypy-prepush-config-baseline-rc1` (`6117fd02f`) | Already an ancestor of the collector; synchronizes branch mypy config into baseline and preserves canonical `vmaf_train.*` identity | Reuse unchanged as the gate authority |

## Decision and event mapping

The hosted job runs `scripts/git-hooks/pre-push-mypy.py`, installed from the
existing hash-locked mypy lock. `actions/checkout` uses `fetch-depth: 0`, whose
documented contract fetches history for all branches and tags. Pull requests
compare with `origin/master`; master pushes set `VMAFX_MYPY_BASE_REF` to
`github.event.before`, preventing the post-merge run from comparing HEAD with
itself. The local hook still defaults to `origin/master`.

The checker keeps `--no-site-packages` and disables only the missing-import
diagnostic caused by that isolation. It still checks repository imports,
standard-library types, and every other enabled strict diagnostic. Paths under
`ai/src/` remain a separate `--explicit-package-bases` invocation.

## Red-cap and mutation evidence

`python3 -B scripts/ci/test_fail_closed_ci.py` failed on the collector parent
because Python Lint had neither full history nor the canonical gate step. Its
mutation matrix now rejects:

1. shallow checkout;
2. installation without `--require-hashes`;
3. loss of exact push-base authority;
4. an advisory `|| echo` tail;
5. restoration of raw `mypy ai/ scripts/` directory discovery.

`scripts/git-hooks/test-pre-push-mypy.py` additionally proves an explicit push
base treats a pre-existing finding as inherited, an invalid explicit base fails
closed, and the default local authority remains unchanged.

## Result

The required job can no longer publish a failed mypy invocation as success.
The production hook has one selection/fingerprint/configuration authority for
both local pushes and hosted CI, and current collector Python changes pass that
authority with zero introduced findings.
