# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

"""core/src/sycl/run_captured.py keeps a successful compile quiet and a failed one loud."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import subprocess
import sys
import unittest
from pathlib import Path
from types import ModuleType
from unittest import mock

SCRIPT = Path(__file__).resolve().parents[3] / "core/src/sycl/run_captured.py"
PYTHON = sys.executable
SCRIPT_TIMEOUT_S = 60


def load_run_captured_module() -> ModuleType:
    """Load core/src/sycl/run_captured.py dynamically via its file location."""
    spec = importlib.util.spec_from_file_location("run_captured", SCRIPT)
    if spec is None or spec.loader is None:
        raise ImportError(f"Cannot load module spec from {SCRIPT}.")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run(*args: str) -> subprocess.CompletedProcess[str]:
    """Execute the run_captured script with arguments and return the completed process."""
    return (
        subprocess.run(  # noqa: S603 -- the interpreter and the script are this repository's own.
            [PYTHON, str(SCRIPT), *args],
            capture_output=True,
            text=True,
            check=False,
            timeout=SCRIPT_TIMEOUT_S,
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

    def test_in_process_positive_records_timeout(self) -> None:
        """Verify successful in-process run returns zero and records the configured timeout."""
        mod = load_run_captured_module()
        fake_result = subprocess.CompletedProcess(["fake", "cmd"], 0, stdout="", stderr="")
        with mock.patch.object(mod.subprocess, "run", return_value=fake_result) as mock_run:
            stdout_buf = io.StringIO()
            stderr_buf = io.StringIO()
            with contextlib.redirect_stdout(stdout_buf), contextlib.redirect_stderr(stderr_buf):
                rc = mod.main(["--", "fake", "cmd"])
            self.assertEqual(rc, 0)
            self.assertEqual(mock_run.call_args.kwargs.get("timeout"), mod.CC_TIMEOUT_S)

    def test_in_process_timeout_str_boundary(self) -> None:
        """Verify string timeout output returns code 124 and echoes stdout and stderr."""
        mod = load_run_captured_module()
        cmd = ["fake", "cmd"]
        timeout_exc = subprocess.TimeoutExpired(cmd, mod.CC_TIMEOUT_S, output="partial")
        with mock.patch.object(mod.subprocess, "run", side_effect=timeout_exc):
            stdout_buf = io.StringIO()
            stderr_buf = io.StringIO()
            with contextlib.redirect_stdout(stdout_buf), contextlib.redirect_stderr(stderr_buf):
                rc = mod.main(["--", *cmd])
            self.assertEqual(rc, 124)
            self.assertIn("timed out after", stderr_buf.getvalue())
            self.assertIn(str(mod.CC_TIMEOUT_S), stderr_buf.getvalue())
            self.assertIn("CC_TIMEOUT_S", stderr_buf.getvalue())
            self.assertIn("partial", stdout_buf.getvalue())

    def test_in_process_timeout_bytes_boundary(self) -> None:
        """Verify byte timeout output decodes without error and returns code 124."""
        mod = load_run_captured_module()
        cmd = ["fake", "cmd"]
        timeout_exc = subprocess.TimeoutExpired(cmd, mod.CC_TIMEOUT_S, output=b"partial\xff")
        with mock.patch.object(mod.subprocess, "run", side_effect=timeout_exc):
            stdout_buf = io.StringIO()
            stderr_buf = io.StringIO()
            with contextlib.redirect_stdout(stdout_buf), contextlib.redirect_stderr(stderr_buf):
                rc = mod.main(["--", *cmd])
            self.assertEqual(rc, 124)
            self.assertIn("partial", stdout_buf.getvalue())

    def test_in_process_timeout_negative_empty_output(self) -> None:
        """Verify timeout with None output returns code 124 and empty stdout."""
        mod = load_run_captured_module()
        cmd = ["fake", "cmd"]
        timeout_exc = subprocess.TimeoutExpired(cmd, mod.CC_TIMEOUT_S, output=None)
        with mock.patch.object(mod.subprocess, "run", side_effect=timeout_exc):
            stdout_buf = io.StringIO()
            stderr_buf = io.StringIO()
            with contextlib.redirect_stdout(stdout_buf), contextlib.redirect_stderr(stderr_buf):
                rc = mod.main(["--", *cmd])
            self.assertEqual(rc, 124)
            self.assertEqual(stdout_buf.getvalue(), "")


if __name__ == "__main__":
    unittest.main()
