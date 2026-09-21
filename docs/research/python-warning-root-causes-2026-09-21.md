<!-- markdownlint-disable MD013 -->
# Research: Python warning root causes (2026-09-21)

- **Status**: Complete
- **Scope**: six warning-heavy classic-harness test modules
- **Decision**: [ADR-1278](../adr/1278-python-safe-parallel-execution.md)

## Question

Which operations generated the Python 3.14/SciPy warning batch, and what changes
remove their causes without suppressing warnings, changing Netflix golden-score
assertions, or allowing duplicate assets to execute concurrently?

## Reproducer and baseline

```bash
PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 python -m pytest -q -W always \
  python/test/cuda_default_model_test.py \
  python/test/executor_test.py \
  python/test/local_explainer_test.py \
  python/test/sycl_motion_parity_test.py \
  python/test/train_test_model_test.py \
  python/test/vmaf_v1_quality_runner_test.py
```

The isolated-worktree baseline reported 334 warnings: one deprecated
`scipy.misc` import, 330 unsafe-fork warnings from
`multiprocessing/popen_fork.py`, two `numpy.exp` overflow warnings, and one
SciPy covariance `OptimizeWarning`. The integration checkout's preceding run
reported 333; both inventories contained the same four root causes and the
same remediation scope.

## Findings

### The import warning was a dead dependency

`tools/sigproc.py` imported `scipy.misc` but never referenced it. Removing the
import removes the warning and changes no execution path.

### The process warning identified a real unsafe operation

`tools/misc.py` forced `multiprocessing` to use `fork` globally, then created a
manager and process pool while the test process had live threads. Python's
documentation states that safely forking a multithreaded process is
problematic; Python 3.12 and newer may emit a `DeprecationWarning`, and Python
3.14 no longer defaults to `fork` on any platform. Filtering that warning would
leave the unsafe inherited thread state in place.

Joblib's `loky` backend starts isolated worker processes and transports local
functions with cloudpickle. A keyed grouping layer preserves the executor's
second invariant: equal asset strings run sequentially while different groups
can run concurrently, and indexed reconstruction retains caller order. FIFO
producer helpers use an explicit `spawn` context because they do not pass
arbitrary local callbacks.

### The 5PL warning exposed the wrong equation

The implementation was effectively
`b1 + sigmoid(x) + b4*x + b5`. Because both `b1` and `b5` were additive
intercepts, the optimizer could not identify them independently and reported an
indeterminate covariance matrix. Equation 3 of Sheikh, Sabir, and Bovik uses
`b1` as the sigmoid amplitude:

```text
Q(x) = b1 * (expit(b2 * (x - b3)) - 0.5) + b4 * x + b5
```

`scipy.special.expit` evaluates the same sigmoid without materializing the
overflowing exponential. A synthetic curve test now proves that all five
parameters contribute, and predictions at `-10000` and `10000` remain finite.

### The fatal-warning gate exposed a temporary-file leak

The broader `tools_test.py` run found that `import_python_file(...,
override=...)` kept its `NamedTemporaryFile` wrapper open while reopening the
path. Python 3.14 reported the implicit garbage-collection cleanup as a
`ResourceWarning`. The override path now closes the wrapper before rewriting
the file and removes the temporary module in a `finally` block, including when
the generated module fails to import.

## Validation contract

- importing `vmaf.tools.sigproc` under `-W error` succeeds;
- `parallel_map` under a live thread emits no warning and retains input order;
- equal asset keys never overlap even when distinct keys run in parallel;
- the established 5PL RMSE assertion remains unchanged and passes under
  warnings-as-errors;
- synthetic 5PL fitting recovers the reference curve without overflow or
  covariance warnings;
- overridden Python-file imports close and remove their temporary file without
  `ResourceWarning`;
- the original six-module census reports zero warnings.

The final CUDA-enabled Python 3.14 run used `-W error` and completed with
31 passed, 7 skipped, and zero warnings. The seven skips were three unavailable
SYCL-device cases and four pre-existing HFR-model `ENOTSUP` cases.

## Regression gate

The root pytest configuration now contains only `filterwarnings = ["error"]`;
there are no `ignore` entries. The legacy tox configuration previously passed
`-p no:warnings`, disabling pytest's warning plugin completely, so GPU CI runs
that select it with `-c python/tox.ini` bypassed the root policy. That switch is
removed and the tox config independently sets `filterwarnings = error`.

The pytest-asyncio 1.4 configuration warning is fixed by declaring
`asyncio_default_fixture_loop_scope = "function"` in each config that loads the
plugin. This makes the intended existing per-test fixture isolation explicit;
it is not a warning filter.

Package-local pytest configurations override discovery from the repository
root when tests run from their package directories. The AI, dev-LLM, MCP,
vmaf-tune, and ROI-score configurations therefore each carry the same fatal
warning policy; MCP also carries the explicit asyncio fixture scope.

## Primary sources

- [Python multiprocessing contexts](https://docs.python.org/3/library/multiprocessing.html#contexts-and-start-methods)
- [joblib serialization and process backends](https://joblib.readthedocs.io/en/stable/auto_examples/serialization_and_wrappers.html)
- [SciPy `expit`](https://docs.scipy.org/doc/scipy/reference/generated/scipy.special.expit.html)
- [SciPy `curve_fit` and redundant-parameter diagnostics](https://docs.scipy.org/doc/scipy/reference/generated/scipy.optimize.curve_fit.html)
- [Sheikh, Sabir, and Bovik, 2006](https://utw10503.utweb.utexas.edu/publications/2006/hrs-transIP-06.pdf)
