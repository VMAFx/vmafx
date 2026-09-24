<!-- markdownlint-disable MD013 -->

# Research-2080: CodeQL Python alerts triage and exception semantics

- **Status**: Active
- **Workstream**: CodeQL Python alert triage (alerts 1275, 1276, 1239)
- **Last updated**: 2026-09-25
- **Date**: 2026-09-24
- **Scope**: Open CodeQL Python alerts on `origin/master`: alert 1275 (`py/empty-except`), alert 1276 (`py/empty-except`), and alert 1239 (`py/test-equals-none`).

---

## 1. Problem Statement & Live Alert Inventory

A live scan of open CodeQL Python alerts on `origin/master` (`gh api /repos/VMAFx/vmafx/code-scanning/alerts?tool_name=CodeQL&state=open`) revealed five alerts:

1. **Alert 1275**: `py/empty-except` at `compat/python-vmaf/core/executor.py:42` — `'except' clause does nothing but pass and there is no explanatory comment.`
2. **Alert 1276**: `py/empty-except` at `compat/python-vmaf/core/executor.py:73` — `'except' clause does nothing but pass and there is no explanatory comment.`
3. **Alert 1239**: `py/test-equals-none` at `compat/python-vmaf/core/train_test_model.py:311` — `Testing for None should use the 'is' operator.`
4. **Alerts 1237 / 917**: `py/cyclic-import` in `mcp-server/vmaf-mcp/` — tracked and resolved independently on branch `fix/mcp-cyclic-imports` (ADR-1304).

Alerts 1275 and 1276 were introduced in commit `ef97f72c8` ("bound FIFO producer startup waits (#1531)"), while alert 1239 was pre-existing and temporarily skipped during prior passes due to perceived NumPy array limitations.

---

## 2. Root Cause Analysis & Intended Semantics

### 2.1 Alert 1275 — `_run_fifo_worker` (`compat/python-vmaf/core/executor.py`)

In `_run_fifo_worker`, a child process runs `target(asset, True, open_sem=ready_sem)`. When `target` raises an application exception:

- The child attempts to transmit the formatted traceback back to the parent via an IPC pipe `error_sender.send(traceback.format_exc())`.
- If the parent process has already closed its end of the pipe (e.g. parent timed out or another worker failed), `error_sender.send()` raises `BrokenPipeError`, `EOFError`, or `OSError`.
- The prior code caught `(BrokenPipeError, EOFError, OSError)` with an empty `pass` before re-raising the primary exception.
- **Intended Semantics**: The primary application failure must propagate and terminate the process with exit code 1; it must never be swallowed or replaced by a secondary `BrokenPipeError` or channel teardown failure. While `origin/master`'s `finally` block already closed the descriptor on normal and exceptional paths, passing inside the handler silently discarded the IPC delivery failure. Furthermore, attempting to close the descriptor directly within the exception handler or allowing a secondary `OSError` during `finally` closure could displace the target exception. The resolved code uses a narrow cleanup helper `_safe_close_channel` in `finally` and attaches diagnostic context through `_safe_add_exception_note`. That helper calls the built-in `BaseException.add_note(...)` when available, bypasses a custom exception's `add_note` override, and treats every failure while storing `__notes__` as secondary, including `BaseException`-derived control-flow failures such as `KeyboardInterrupt`, so send, note, or close handling cannot replace the target exception while resources remain bounded.

### 2.2 Alert 1276 — `_fifo_worker_failure` (`compat/python-vmaf/core/executor.py`)

In `_fifo_worker_failure`, the parent polls `error_receiver.poll()`:

- In Python multiprocessing, `poll()` returns `True` both when data is available and when the pipe reaches EOF (write-end closed).
- When the child process terminates abruptly without sending an error payload, `error_receiver.recv()` raises `EOFError`.
- The prior code caught `EOFError` with `pass`, leaving `child_traceback = None`. If the child's `process.exitcode` had not yet been harvested by `waitpid`, `_fifo_worker_failure` evaluated `child_traceback is None and process.exitcode is None` to `True` and returned `None`. This falsely indicated the worker was still healthy and running, delaying failure detection.
- **Intended Semantics**: Channel EOF without an error payload means the child process closed its write end without signaling readiness. It is a definite worker failure. The resolved code populates `child_traceback` with `"<unavailable from child error channel (EOF); inspect the inherited child stderr>"`, which immediately triggers `process.join()` and raises `RuntimeError` rather than delaying. Rather than conflating channel EOF with OS-level pipe errors, `_fifo_worker_failure` distinguishes `EOFError` (`(EOF)`) from `OSError` diagnostics (`(OSError: ...)`), testing both paths.

### 2.3 Alert 1239 — `_get_scatter_arrays` (`compat/python-vmaf/core/train_test_model.py`)

In `RegressorMixin._get_scatter_arrays`, `stats["ys_label_stddev"]` may contain Python `None` values (e.g. uncalibrated or missing stimulus standard deviations).

- `np.array(stats["ys_label_stddev"])` creates an array with `dtype=object`.
- `np.isnan(ys_label_stddev)` raises `TypeError` because `ufunc 'isnan'` does not support object arrays containing `None`.
- The prior code fell back to `ys_label_stddev[ys_label_stddev == None] = 0 # noqa: E711`. CodeQL flagged `== None` under rule `py/test-equals-none`.
- **Intended Semantics**: The comparison must use the identity operator `is None` rather than equality `== None`. The resolved code constructs a boolean mask using `[x is None for x in ys_label_stddev.flat].reshape(...)`, zeroes those elements, converts the array to float, and zeroes any floating-point `NaN` values. This completely eliminates `== None`, removes `# noqa: E711`, and preserves exact numerical behavior.

---

## 3. Auditing Nearby Occurrences

An AST search across `compat/python-vmaf/` audited all `except ...: pass` occurrences:

- `compat/python-vmaf/config.py:67`: `except OSError:` during temp file removal already carried an explanatory comment and was clean.
- `compat/python-vmaf/core/nn_train_test_model.py:177, 190`: `except KeyError:` on initial dataset creation already carried explanatory comments and was clean.
- `compat/python-vmaf/tools/misc.py:459`: `except (FormatError, IncompleteCaptureError):` in `check_scanf_match` fell through to `fnmatch`; documented with an explicit explanatory comment.
- `compat/python-vmaf/tools/scanf.py:310`: `except (IOError, OSError, ValueError):` in `isFileLike` fell through to `return False`; documented with an explicit explanatory comment.

---

## 4. Alternatives Considered & Decision Matrix

| Option / Surface | Pros | Cons | Decision |
| :--- | :--- | :--- | :--- |
| **Alert 1275** (`_run_fifo_worker`): Swallow IPC failure with `pass` | Zero lines added | Discards IPC delivery context (even though descriptor closed in `finally`) | Rejected |
| **Alert 1275**: Replace target exception with `BrokenPipeError` | Surfaces IPC error | Masks original application error that caused the exit | Rejected |
| **Alert 1275**: Narrow cleanup and fail-soft exception-note helpers (chosen) | Preserves primary exception even when its class overrides `add_note` or note storage fails, guarantees close/send errors never mask target failure, bounds resources | None | **Adopted** |
| **Alert 1276** (`_fifo_worker_failure`): Ignore EOF with `pass` | Low-complexity | Delays child exit detection until process exit reap | Rejected |
| **Alert 1276**: Synthesize child traceback on EOF / OSError channel (chosen) | Immediate failure propagation, prevents supervisor stalls, distinguishes EOF from OSError | None | **Adopted** |
| **Alert 1239** (`_get_scatter_arrays`): Keep `== None` with `# noqa: E711` | Minimal diff | Violates CodeQL rule, calls `__eq__` on unknown objects | Rejected |
| **Alert 1239**: Identity mask `x is None` + NaN zeroing (chosen) | Conforms to Python semantics, handles mixed None/NaN, eliminates `# noqa` | None | **Adopted** |

---

## 5. Verification and Governance

- **Focused Unit Tests**:
  - `python/test/executor_test.py`: Real multiprocessing `Pipe` red-cap regression testing POSIX descriptor invalidation via `os.close(sender.fileno())`, portable closed handles and broken pipes, a custom exception whose `add_note` override raises, and custom `__setattr__` implementations that reject `__notes__` storage with both `Exception`- and `BaseException`-derived failures. These prove that the target exception remains primary and that runtimes providing a usable built-in note API attach the diagnostic. The raw-descriptor probe is skipped off POSIX because Windows `multiprocessing.Pipe` exposes native handles rather than file descriptors. Tested immediate EOF on `_fifo_worker_failure` with a Linux `/proc/self/fd` child probe where `process.exitcode` initially is `None` and `join` transitions state to 42; that probe is skipped when `/proc/self/fd` is unavailable. Verified faithful state transitions distinguishing `EOFError` (`(EOF)`) from `OSError` (`(OSError: ...)`) diagnostics.
  - `python/test/train_test_model_test.py`: Added `GetScatterArraysTest` with `StrictNoEqualityToNone` sentinel (verifying `== None` is never invoked), mixed None/NaN zeroing, all-None, all-NaN, and error cases.
  - `python/test/python_harness_scanf_locale_bugs_test.py`: Added `TestIsFileLike` (verifying `(IOError, OSError, ValueError)` safe returns) and `TestCheckScanfMatchFallback`.
- **CodeQL Evaluation**:
  - Evaluated against a freshly generated exact-head database using CodeQL 2.27.0 with `codeql/python-queries/1.8.10/Exceptions/EmptyExcept.ql` and `Expressions/EqualsNone.ql`.
  - Result: **0 alerts** in `compat/python-vmaf/` and across the repository (alerts 1275, 1276, and 1239 completely resolved).
- **Linters**:
  - `ruff check`: PASS (zero findings).
  - `black --check`: PASS (zero changes required).
- **HISS-21 Governance**:
  - `praetorctl audit`: PASS (zero touched-file findings; 276/276 baseline).
  - `praetorctl compile-context --verify`: PASS (all targets in sync).
  - `scripts/ci/check-state-md-rows.sh`: PASS.
