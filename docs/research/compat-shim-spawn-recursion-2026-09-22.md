<!-- markdownlint-disable MD013 -->
# Research digest: the `vmaf` shim deadlock in multiprocessing spawn children

- **Date**: 2026-09-22
- **Decision recorded in**: [ADR-1292](../adr/1292-compat-shim-spawn-safe.md)
- **Code**: `python/vmaf/__init__.py`, `compat/python-vmaf/core/executor.py`

## Symptom

`Ubuntu gcc` and `Ubuntu clang` were cancelled with no failing assertion. The
job log shows the last test output at 12:27 and 12:30, then
`##[error]The operation was canceled` at 13:32:47 — a 62-minute gap with no
output. Both legs stop at the same point: the final test of
`python/test/quality_runner_test.py`
(`QualityRunnerSaveWorkfilesTest::test_run_vmaf_runner_flat_save_workfiles`)
passes, then silence. That test is test 62 of 62 in its file, so the stall is at
the file boundary, not inside the file — `pytest python/test/quality_runner_test.py`
alone is `61 passed, 1 skipped in 53.20s`.

`pytest python/test --collect-only` gives the next file as
`python/test/raw_extractor_test.py`.

## Reproduction

```text
$ cd python && pytest test/raw_extractor_test.py -x -q -s -p no:randomly
   (no output; killed at 420 s, and again at 90 s and 70 s)
```

`faulthandler` in the parent:

```text
File ".../compat/vmaf/core/executor.py", line 426 in _open_workfiles_in_fifo_mode
File ".../compat/vmaf/core/executor.py", line 354 in _prepare_asset
File ".../compat/vmaf/core/executor.py", line 383 in _calculate_result
File ".../compat/vmaf/core/executor.py", line 397 in _run_on_asset
File ".../compat/vmaf/core/executor.py", line 192 in run
File ".../python/test/raw_extractor_test.py", line 42 in test_run_asset_extractor
```

Line 426 is the unconditional `sem.acquire()` inside the ">5 seconds elapsed"
branch — the parent already waited out its 5-second timeout and is now blocked
with no deadline.

With `-s`, the child's stderr is not swallowed:

```text
File "/usr/lib/python3.14/multiprocessing/spawn.py", line 132, in _main
  self = reduction.pickle.load(from_parent)
File ".../python/vmaf/__init__.py", line 27, in <module>
  importlib.import_module(__name__)
   ... repeated ...
RecursionError: maximum recursion depth exceeded
```

## Mechanism

The shim redirected by mutating the import system:

```python
if _compat_dir not in sys.path:
    sys.path.insert(0, _compat_dir)
del sys.modules[__name__]
importlib.import_module(__name__)
```

The redirect works only if `compat/` precedes `python/` on `sys.path`. The guard
tests membership, not precedence. Instrumenting the shim to dump its state on
re-entry gives the child's actual path:

```text
SHIMPROBE depth= 2 path= ['.../python', '.../compat', '.../python', ...]
SHIMPROBE spec= ModuleSpec(name='vmaf', origin='.../python/vmaf/__init__.py', ...)
```

`python/` is at index 0 and `compat/` at index 1, so `find_spec("vmaf")` returns
the shim. `compat/` *is* in `sys.path`, so the insert is skipped, and the
re-import resolves back to the shim — unbounded.

The child reaches this during `spawn._main`: `prepare()` restores the parent's
`sys.path`, then `reduction.pickle.load()` unpickles the bound method
`self._open_ref_workfile`, which imports `vmaf.core.executor` from scratch. The
child dies before `_open_workfile` reaches `open_sem.release()`, so the parent's
`sem.acquire()` never returns.

The parent never recurses: its first `import vmaf` runs while
`sys.modules['vmaf']` is empty, so the insert happens and `compat/` moves to the
front. The defect is only reachable in a freshly started interpreter.

## Why master is unaffected

[ADR-1278](../adr/1278-python-safe-parallel-execution.md) introduced
`_MULTIPROCESSING_CONTEXT = multiprocessing.get_context("spawn")` in
`compat/python-vmaf/core/executor.py` (commit `9dd360d5f`). Before it, these
helpers used the interpreter default, which on Python 3.14 / Linux is
`forkserver`. A forkserver child is forked and inherits `sys.modules`, so it
never re-imports `vmaf`. Only `spawn` starts a fresh interpreter. The shim bug
predates ADR-1278; ADR-1278 is what made it reachable.

The earlier `cannot find context for 'loky'` hang fixed in `ed4fe4e4a` is a
different failure of the same nested-process path and is not superseded by this
change.

## Verification

```text
$ pytest python/test/raw_extractor_test.py -q -p no:randomly
4 passed in 1.44s
```

Module identity is unchanged by the new loader:

```text
vmaf.__file__  = .../compat/vmaf/__init__.py
vmaf.__path__  = ['.../compat/vmaf']
vmaf.core.executor.__name__ = vmaf.core.executor
VmafConfig.root_path() = <repo root>
```

## Residual

`_open_workfiles_in_fifo_mode` and `_open_procfiles_in_fifo_mode` both end in a
bare `sem.acquire()` after their warning branch. Any child that dies before
releasing hangs the parent until the CI job's own ceiling rather than failing
with the child's exit code. Tracked separately in `docs/state.md`.
