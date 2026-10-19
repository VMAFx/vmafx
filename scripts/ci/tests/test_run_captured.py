# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

"""core/src/sycl/run_captured.py keeps a successful compile quiet and a failed one loud."""

from __future__ import annotations

import subprocess
import sys
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[3] / "core/src/sycl/run_captured.py"
PYTHON = sys.executable


def run(*args: str) -> subprocess.CompletedProcess[str]:
    return (
        subprocess.run(  # noqa: S603 -- the interpreter and the script are this repository's own.
            [PYTHON, str(SCRIPT), *args], capture_output=True, text=True, check=False
        )
    )


class RunCapturedTests(unittest.TestCase):
    def test_success_prints_nothing(self) -> None:
        noisy = "import sys; print('warning: spilled'); print('more', file=sys.stderr)"
        result = run("--", PYTHON, "-c", noisy)
        self.assertEqual((result.returncode, result.stdout, result.stderr), (0, "", ""))

    def test_failure_keeps_stdout_and_stderr_and_the_exit_status(self) -> None:
        loud = "import sys; print('out'); print('err', file=sys.stderr); sys.exit(3)"
        result = run("--", PYTHON, "-c", loud)
        self.assertEqual(result.returncode, 3)
        self.assertIn("out", result.stdout)
        self.assertIn("err", result.stdout)

    def test_missing_separator_or_command_is_a_usage_error(self) -> None:
        for args in ((), ("--",), (PYTHON, "-c", "pass")):
            with self.subTest(args=args):
                self.assertEqual(run(*args).returncode, 2)


if __name__ == "__main__":
    unittest.main()
