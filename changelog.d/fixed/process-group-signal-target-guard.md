- Process control: every place that signals a child's process group now goes
  through `signal_process_group()` in `scripts/lib/safe_subprocess.py`, which
  refuses a target that is not an `int` above 1 or that is the caller's own
  group before any signal is sent. `os.killpg(1, sig)` is `kill(-1, sig)` and
  reaches every process the user owns, and a test double's pid indexes to 1.
  The actionlint wrapper, the `rc1-tester` copy of the module and two tests use
  the guard (#2847).
