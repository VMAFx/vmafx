## Process-group signals go through one guard (2026-10-18)

`fix/signal-target-guard` (#2847). Fork-only scripts and tests: no upstream file
is touched. A rebase that brings a new `os.killpg()` or `os.kill(-pid, ...)` call
routes it through `scripts.lib.safe_subprocess.signal_process_group()` instead;
signal-0 probes (`_posix_process_group_exists()`) stay as they are.
`tools/rc1-tester/src/vmaf_rc1_tester/_safe_subprocess.py` stays a byte copy of
`scripts/lib/safe_subprocess.py` (`test_safe_subprocess_copy.py`).
