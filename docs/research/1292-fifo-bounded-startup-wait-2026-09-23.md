# Research-1292: Bounded FIFO producer startup wait

## Scope

This digest covers BUG-090 only: startup supervision for the FIFO workfile and
procfile producers in `compat/python-vmaf/core/executor.py`. The compatibility
shim failure that exposed the hang is a separate defect documented in
[the spawn-recursion digest](compat-shim-spawn-recursion-2026-09-22.md) and
[ADR-1292](../adr/1292-compat-shim-spawn-safe.md).

## Reproducer and root cause

Before this fix, each two-input helper created one shared semaphore, started
the reference and distorted producers, waited five seconds for one release,
then called `sem.acquire()` without a timeout for the remaining releases.
`NorefExecutorMixin` had the same sequence for its single distorted producer.
The five-second branch only logged that it was "now blocking until created";
it did not inspect `Process.exitcode` or `Process.is_alive()`.

A producer that raised before `open_sem.release()` therefore became
indistinguishable from a healthy but slow producer. The parent had no deadline,
no attribution of a readiness release to a specific child, and no channel for
the child traceback. The observed BUG-089 spawn failure demonstrated the
consequence: the child exited with `RecursionError`, while the parent remained
inside the unconditional acquire until the outer CI job was cancelled.

The regression tests make both failure modes deterministic with real `spawn`
children:

- `test_fifo_helpers_surface_child_failure` raises before readiness in the
  base and no-reference workfile and procfile paths. Each parent must receive a
  `RuntimeError` naming the producer role, exit code, and child traceback.
- `test_fifo_helpers_bound_live_child_wait` uses producers that stay alive but
  never signal. It shortens the production deadline to 0.25 seconds inside an
  isolated spawn parent and requires `TimeoutError` naming both pending roles.

## Options

| Approach | Dead child visible | Slow healthy child preserved | Readiness attributable | Decision |
| --- | --- | --- | --- | --- |
| Keep the shared semaphore and add only a hard timeout | Only after the deadline | Yes | No | Rejected: diagnosis remains delayed and ambiguous. |
| Poll both exit codes around the shared semaphore | Promptly | Yes | No | Rejected: either child can consume either release. |
| Give each child a readiness semaphore and one-way error pipe | Promptly, with target traceback when available | Yes, until the hard deadline | Yes | Selected. |

This is a bounded bug fix within ADR-1278's existing explicit `spawn` process
model, not a new concurrency architecture.

## Implementation

`_start_fifo_worker()` creates one semaphore and one one-way pipe per producer.
`_run_fifo_worker()` invokes the existing target, sends `traceback.format_exc()`
when target code raises, closes its pipe endpoint, and re-raises so the process
exit code remains non-zero. A failure during spawn bootstrap occurs before that
wrapper can run; in that case the inherited child stderr remains the detailed
diagnostic and the parent still reports the non-zero exit code.

`_wait_for_fifo_workers()` uses `time.monotonic()` for elapsed time, polls every
50 ms, emits the existing slow-start warning after five seconds, and raises at
60 seconds. Python documents `monotonic()` as unaffected by system-clock
updates, which makes it suitable for a deadline. The loop also checks each
pending child's error pipe and exit code before the next bounded wait.

On any startup failure, `_stop_fifo_workers()` terminates live siblings, joins
them for one second, then uses `kill()` only for a child that did not exit. The
parent closes every receive endpoint in `finally`. Python's multiprocessing
contract defines `exitcode is None` while a process is alive and exit code 1
for an uncaught child exception; it also requires the creating process to call
`join`, `is_alive`, `terminate`, and inspect `exitcode`, which is exactly where
the supervisor performs those operations.

Primary API references:

- [Python 3 multiprocessing process and connection APIs](https://docs.python.org/3/library/multiprocessing.html)
- [Python 3 monotonic clock](https://docs.python.org/3/library/time.html#time.monotonic)

## Evidence

```text
PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 python3 -m pytest -q -W error \
  python/test/executor_test.py
4 passed, 4 subtests passed

PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 python3 -m pytest -q -W error \
  python/test/raw_extractor_test.py
4 passed
```

The first command covers failed children and the live-never-ready deadline for
both `Executor` and `NorefExecutorMixin`. The second preserves a healthy real
FIFO path. These results establish local process behavior; hosted platform
coverage remains the post-push CI responsibility.

## Limits and retained behavior

- The five-second event remains a warning, not a failure; a healthy producer
  has the rest of the 60-second startup window.
- The 60-second value bounds producer readiness only. It does not bound the
  later scoring workload or change FIFO data flow.
- The error pipe captures exceptions raised after the worker wrapper starts.
  Interpreter/bootstrap failures cannot use that pipe and remain visible on
  inherited stderr plus the parent's exit-code report.
- Forced termination is confined to startup failure. The parent discards the
  readiness and diagnostic channels afterward rather than reusing resources
  that Python warns may be left unusable when a process is terminated.
- Netflix golden assertions and score computation are untouched.
