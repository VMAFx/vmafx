#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The Meson test runner must return Meson's exit status and wait for it (ADR-1364).

scripts/ci/run_meson_test.py replaced itself with ``meson test`` through
``os.execvp``. Windows has no exec: the CRT starts the new program and ends
the caller with status 0 at once, so the Windows MinGW64 and ARM64 MSVC lanes
reported success after the first few tests while the suite was still running.
A stand-in "meson" (the Python interpreter running a script named ``test``)
exits with a chosen status after a delay; the runner must return that status,
and only after the stand-in has finished.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

RUNNER = Path(__file__).resolve().parents[2] / "scripts" / "ci" / "run_meson_test.py"
DELAY_SECONDS = 1.0


def run_runner(status: int, *extra: str) -> tuple[int, float, str]:
    """Run the runner with a stand-in Meson that exits ``status``."""
    # A runner that returns early leaves the stand-in running in the directory;
    # let that surface as the status assertion, not as a cleanup error.
    with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as directory:
        workdir = Path(directory)
        marker = workdir / "finished"
        # `meson test ARGS` becomes `python test ARGS`: this file is the script.
        (workdir / "test").write_text(
            "import pathlib, sys, time\n"
            f"time.sleep({DELAY_SECONDS})\n"
            f"pathlib.Path({str(marker)!r}).write_text(' '.join(sys.argv[1:]))\n"
            f"sys.exit({status})\n",
            encoding="utf-8",
        )
        started = time.monotonic()
        # Fixed argv: this interpreter, the repository runner and test-owned args.
        completed = subprocess.run(  # noqa: S603
            [sys.executable, str(RUNNER), "--meson-executable", sys.executable, "--", *extra],
            cwd=workdir,
            check=False,
            timeout=60,
        )
        elapsed = time.monotonic() - started
        seen = marker.read_text(encoding="utf-8") if marker.exists() else "<not finished>"
        return completed.returncode, elapsed, seen


class RunMesonTestExitStatusTest(unittest.TestCase):
    def test_failing_suite_status_reaches_the_caller(self) -> None:
        status, elapsed, seen = run_runner(3, "--suite", "fast")
        self.assertEqual(status, 3, "a failing meson test must fail the caller")
        self.assertGreaterEqual(elapsed, DELAY_SECONDS * 0.9)
        self.assertEqual(seen, "--suite fast", "runner returned before Meson finished")

    def test_passing_suite_returns_zero_after_meson_finishes(self) -> None:
        status, elapsed, seen = run_runner(0)
        self.assertEqual(status, 0)
        self.assertGreaterEqual(elapsed, DELAY_SECONDS * 0.9)
        self.assertEqual(seen, "")

    def test_missing_meson_is_reported_not_passed(self) -> None:
        completed = subprocess.run(  # noqa: S603 - fixed, test-owned argv
            [sys.executable, str(RUNNER), "--meson-executable", "no-such-meson-binary", "--"],
            check=False,
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(completed.returncode, 127)
        self.assertIn("Meson executable not found", completed.stderr)


if __name__ == "__main__":
    unittest.main()
