<!-- markdownlint-disable MD013 -->
# Python process execution

The classic VMAF Python harness runs independent assets in separate processes.
Its process boundary has three guarantees:

1. `parallel_map()` accepts local functions and returns values in input order.
2. Executor entries with the same `str(asset)` key never run concurrently,
   because they may share workfile and result-store paths.
3. Starting work while another thread is live never invokes POSIX `fork()`.

`vmaf.tools.misc.parallel_map` therefore uses joblib's `loky` backend. Executor
calls group equal asset keys into one serial work unit before dispatch and then
restore the original index order. The worker count is capped by the number of
submitted units, so a high-core-count host does not start idle workers. FIFO
producer processes are simpler bound method calls and use an explicit
`multiprocessing` `spawn` context.

Do not replace these paths with the global multiprocessing context, a raw
`fork` pool, or independently dispatched duplicate assets. Python 3.14 warns
about forking a multithreaded process because the child can inherit locks held
by threads that no longer exist.

## Verification

Run the focused concurrency regressions with warnings promoted to errors:

```bash
PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 python -m pytest -q -W error \
  python/test/tools_test.py::MiscTest::test_parallel_map_is_warning_free_with_live_thread_and_preserves_order \
  python/test/executor_test.py::ExecutorTest::test_parallel_run_serializes_duplicate_assets_and_preserves_order
```

Run the complete warning census from the
[research digest](../research/python-warning-root-causes-2026-09-21.md#reproducer-and-baseline)
after building `core/build/tools/vmaf`.

Warnings-as-errors is also the repository default: root `pyproject.toml` and
the legacy `python/tox.ini` both set pytest's sole warning action to `error`.
There are deliberately no `ignore` rules. Package-local AI and dev-LLM pytest
configs carry the same policy, as do the MCP, vmaf-tune, and ROI-score configs.
Configs that load pytest-asyncio pin its fixture loop to function scope so
plugin behavior is explicit.

## Five-parameter logistic fitting

The same regression batch covers classic 5PL calibration. The implementation
uses the Sheikh/Sabir/Bovik equation and `scipy.special.expit`; replacing it
with `numpy.exp` reintroduces overflow at large magnitudes, while making `b1`
additive makes it redundant with `b5` and produces SciPy covariance warnings.
See [ADR-1278](../adr/1278-python-safe-parallel-execution.md).
