# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

"""core/src/sycl/run_captured.py keeps a successful compile quiet and a failed one loud."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import subprocess  # nosec B404 -- launch the fixed repository script in integration tests.
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
    return subprocess.run(  # noqa: S603  # nosec B603 -- fixed interpreter and repository script.
        [PYTHON, str(SCRIPT), *args],
        capture_output=True,
        text=True,
        check=False,
        timeout=SCRIPT_TIMEOUT_S,
    )


class RunCapturedTests(unittest.TestCase):
    def assert_run_options(self, mock_run: mock.Mock, mod: ModuleType, cmd: list[str]) -> None:
        """Check the bounded compile options passed by every in-process path."""
        options = mock_run.call_args.kwargs
        self.assertEqual(mock_run.call_args.args, (cmd,))
        self.assertEqual(options["allowed_executables"], [cmd[0]])
        self.assertIs(options["stderr_to_stdout"], True)
        self.assertEqual(options["max_output_bytes"], mod.MAX_COMPILE_OUTPUT_BYTES)
        self.assertEqual(options["timeout_seconds"], mod.CC_TIMEOUT_S)

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
        with mock.patch.object(mod, "run_command", return_value=fake_result) as mock_run:
            stdout_buf = io.StringIO()
            stderr_buf = io.StringIO()
            with contextlib.redirect_stdout(stdout_buf), contextlib.redirect_stderr(stderr_buf):
                rc = mod.main(["--", "fake", "cmd"])
            self.assertEqual(rc, 0)
            self.assert_run_options(mock_run, mod, ["fake", "cmd"])

    def test_in_process_timeout_str_boundary(self) -> None:
        """Verify string timeout output returns code 124 and echoes stdout and stderr."""
        mod = load_run_captured_module()
        cmd = ["fake", "cmd"]
        timeout_exc = mod.CommandTimedOut(tuple(cmd), mod.CC_TIMEOUT_S, "partial", None)
        with mock.patch.object(mod, "run_command", side_effect=timeout_exc) as mock_run:
            stdout_buf = io.StringIO()
            stderr_buf = io.StringIO()
            with contextlib.redirect_stdout(stdout_buf), contextlib.redirect_stderr(stderr_buf):
                rc = mod.main(["--", *cmd])
            self.assertEqual(rc, 124)
            self.assertIn("timed out after", stderr_buf.getvalue())
            self.assertIn(str(mod.CC_TIMEOUT_S), stderr_buf.getvalue())
            self.assertIn("CC_TIMEOUT_S", stderr_buf.getvalue())
            self.assertIn("partial", stdout_buf.getvalue())
            self.assert_run_options(mock_run, mod, cmd)

    def test_in_process_timeout_bytes_boundary(self) -> None:
        """Verify byte timeout output decodes without error and returns code 124."""
        mod = load_run_captured_module()
        cmd = ["fake", "cmd"]
        timeout_exc = mod.CommandTimedOut(tuple(cmd), mod.CC_TIMEOUT_S, b"partial\xff", None)
        with mock.patch.object(mod, "run_command", side_effect=timeout_exc) as mock_run:
            stdout_buf = io.StringIO()
            stderr_buf = io.StringIO()
            with contextlib.redirect_stdout(stdout_buf), contextlib.redirect_stderr(stderr_buf):
                rc = mod.main(["--", *cmd])
            self.assertEqual(rc, 124)
            self.assertIn("partial", stdout_buf.getvalue())
            self.assert_run_options(mock_run, mod, cmd)

    def test_in_process_timeout_negative_empty_output(self) -> None:
        """Verify timeout with None output returns code 124 and empty stdout."""
        mod = load_run_captured_module()
        cmd = ["fake", "cmd"]
        timeout_exc = mod.CommandTimedOut(tuple(cmd), mod.CC_TIMEOUT_S, None, None)
        with mock.patch.object(mod, "run_command", side_effect=timeout_exc) as mock_run:
            stdout_buf = io.StringIO()
            stderr_buf = io.StringIO()
            with contextlib.redirect_stdout(stdout_buf), contextlib.redirect_stderr(stderr_buf):
                rc = mod.main(["--", *cmd])
            self.assertEqual(rc, 124)
            self.assertEqual(stdout_buf.getvalue(), "")
            self.assert_run_options(mock_run, mod, cmd)

    def test_in_process_output_limit_keeps_retained_stdout(self) -> None:
        """Report the compile limit and retain diagnostics before returning code 125."""
        mod = load_run_captured_module()
        cmd = ["fake", "cmd"]
        limit_exc = mod.CommandOutputLimitExceeded(
            tuple(cmd), mod.MAX_COMPILE_OUTPUT_BYTES, b"partial\xff", None
        )
        with mock.patch.object(mod, "run_command", side_effect=limit_exc) as mock_run:
            stdout_buf = io.StringIO()
            stderr_buf = io.StringIO()
            with contextlib.redirect_stdout(stdout_buf), contextlib.redirect_stderr(stderr_buf):
                rc = mod.main(["--", *cmd])
            self.assertEqual(rc, 125)
            self.assertEqual(stdout_buf.getvalue(), "partial\ufffd")
            self.assertEqual(
                stderr_buf.getvalue(),
                "run_captured: command exceeded "
                f"{mod.MAX_COMPILE_OUTPUT_BYTES} captured bytes (MAX_COMPILE_OUTPUT_BYTES): {cmd}\n",
            )
            self.assert_run_options(mock_run, mod, cmd)

    def test_in_process_validation_failure_has_no_traceback(self) -> None:
        """Show a missing executable as one diagnostic line and return code 126."""
        mod = load_run_captured_module()
        cmd = ["missing", "cmd"]
        reason = "executable is unavailable: missing"
        with mock.patch.object(
            mod, "run_command", side_effect=mod.CommandValidationError(reason)
        ) as mock_run:
            stdout_buf = io.StringIO()
            stderr_buf = io.StringIO()
            with contextlib.redirect_stdout(stdout_buf), contextlib.redirect_stderr(stderr_buf):
                rc = mod.main(["--", *cmd])
            self.assertEqual(rc, 126)
            self.assertEqual(stdout_buf.getvalue(), "")
            self.assertEqual(stderr_buf.getvalue(), f"run_captured: {reason}\n")
            self.assert_run_options(mock_run, mod, cmd)

    def test_in_process_launch_failure_has_no_traceback(self) -> None:
        """Show an operating-system launch failure as one diagnostic line."""
        mod = load_run_captured_module()
        reason = "launch failed"
        with mock.patch.object(mod, "run_command", side_effect=OSError(reason)) as mock_run:
            stdout_buf = io.StringIO()
            stderr_buf = io.StringIO()
            with contextlib.redirect_stdout(stdout_buf), contextlib.redirect_stderr(stderr_buf):
                rc = mod.main(["--", "fake"])
            self.assertEqual(rc, 126)
            self.assertEqual(stdout_buf.getvalue(), "")
            self.assertEqual(stderr_buf.getvalue(), f"run_captured: {reason}\n")
            self.assert_run_options(mock_run, mod, ["fake"])


if __name__ == "__main__":
    unittest.main()
