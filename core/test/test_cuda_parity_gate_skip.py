#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""test_cuda_parity_gate_default_run must skip where CUDA cannot run at all.

The default-run test compares CPU with CUDA on a device. It is registered for
every build, so on a libvmaf built without CUDA (a HIP-only or SYCL-only build
dir) the CLI refuses `--backend cuda` (ADR-0498) and the test used to report
that refusal as a failure of the parity gate. This device-free test pins the
skip decision to the message the CLI actually prints.

It also pins that a build directory configured without CUDA is recognised from
Meson's own record, which the default-run test reads before it waits for the
device lock.

Last, it pins the timeout of the default run: a hung gate returns 124 with a
message naming the bound, the bound sits under the Meson timeout of the test,
and flock execs the gate so the kill reaches the gate and frees the lock.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import os
import re
import shutil
import signal
import subprocess  # nosec B404 -- fixed gate supervision results and timeout test doubles.
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from types import ModuleType
from typing import Any
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_RUN = ROOT / "core" / "test" / "test_cuda_parity_gate_default_run.py"
MESON_BUILD = ROOT / "core" / "test" / "meson.build"
CLI = ROOT / "core" / "tools" / "vmaf.cpp"

# The refusal in validate_requested_backend(), split over two C string literals.
REFUSAL = re.compile(
    r'"(vmaf: --backend %s requested but this libvmaf was built without %s )"\s*'
    r'"(support; refusing to silently fall back to CPU \(ADR-0498\))\\n"'
)


def load_default_run() -> ModuleType:
    """Load the repository parity wrapper for device-free tests."""
    spec = importlib.util.spec_from_file_location("parity_gate_default_run", DEFAULT_RUN)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot load {DEFAULT_RUN}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def cli_refusal(backend: str) -> str:
    match = REFUSAL.search(CLI.read_text(encoding="utf-8"))
    if match is None:
        raise AssertionError("vmaf.cpp no longer prints the built-without-backend refusal")
    return (match.group(1) + match.group(2)) % (backend, backend)


class CudaParityGateSkipTest(unittest.TestCase):
    def test_build_without_cuda_is_a_skip(self) -> None:
        gate_cell = (
            "adm            cpu    ↔ cuda    tol=5.0e-05 (default)  max_abs_diff=0.000e+00  "
            f"ERROR  (backend_b cuda failed: {cli_refusal('cuda')})"
        )
        self.assertTrue(load_default_run().cuda_unavailable(gate_cell))

    def test_missing_device_is_a_skip(self) -> None:
        module = load_default_run()
        self.assertTrue(module.cuda_unavailable("No CUDA device found"))
        self.assertTrue(module.cuda_unavailable("init failed: cudaErrorNoDevice"))

    def test_a_parity_failure_is_not_a_skip(self) -> None:
        module = load_default_run()
        cell = "adm            cpu    ↔ cuda    tol=5.0e-05 (default)  max_abs_diff=3.1e-02  FAIL"
        self.assertFalse(module.cuda_unavailable(cell))
        # Another backend missing from the build says nothing about CUDA.
        self.assertFalse(module.cuda_unavailable(cli_refusal("sycl")))


def build_dir_with(options: str | None) -> tempfile.TemporaryDirectory[str]:
    """A build directory whose Meson option record holds `options`, or none."""
    build = tempfile.TemporaryDirectory()
    if options is not None:
        info = Path(build.name) / "meson-info"
        info.mkdir()
        (info / "intro-buildoptions.json").write_text(options, encoding="utf-8")
    return build


def make_cli(build: Path) -> Path:
    """Create an empty executable `tools/vmaf` under `build` and return its path."""
    binary = build / "tools" / "vmaf"
    binary.parent.mkdir(parents=True)
    binary.write_text("", encoding="utf-8")
    binary.chmod(0o755)
    return binary


class FindBinaryTest(unittest.TestCase):
    """The CLI comes from scripts/lib/vmaftest.py, not from a lookup of this test's own."""

    def run_with(self, root: Path, env: dict[str, str]) -> tuple[ModuleType, Path | None]:
        """find_binary() with `root` as repository root and `env` as the whole environment."""
        module = load_default_run()
        with mock.patch.object(module, "ROOT", root), mock.patch.dict(os.environ, env, clear=True):
            found: Path | None = module.find_binary()
        return module, found

    def test_vmaf_bin_names_the_cli(self) -> None:
        """VMAF_BIN wins over a build directory, so a CUDA build can be named."""
        with tempfile.TemporaryDirectory() as root, tempfile.TemporaryDirectory() as cuda:
            make_cli(Path(root) / "build")
            binary = make_cli(Path(cuda))
            self.assertEqual(self.run_with(Path(root), {"VMAF_BIN": str(binary)})[1], binary)

    def test_build_is_the_fallback(self) -> None:
        """Without a variable, the CLI under the repository's `build/` is used."""
        with tempfile.TemporaryDirectory() as root:
            binary = make_cli(Path(root) / "build")
            self.assertEqual(self.run_with(Path(root), {})[1], binary)

    def test_no_cli_is_a_skip(self) -> None:
        """No variable and no build: main() skips with the resolver's message."""
        with tempfile.TemporaryDirectory() as root:
            module, found = self.run_with(Path(root), {})
            self.assertIsNone(found)
            self.assertEqual(module.skip_reason(None), module.vmaftest.MISSING_MESSAGE)

    def test_invalid_vmaf_bin_is_a_failure(self) -> None:
        """A VMAF_BIN naming no executable fails the run, not skips it or falls through."""
        module = load_default_run()
        with tempfile.TemporaryDirectory() as root:
            make_cli(Path(root) / "build")
            missing = str(Path(root) / "missing" / "vmaf")
            stderr = io.StringIO()
            with (
                mock.patch.object(module, "ROOT", Path(root)),
                mock.patch.dict(os.environ, {"VMAF_BIN": missing}, clear=True),
                contextlib.redirect_stderr(stderr),
            ):
                self.assertEqual(module.main(), 1)
            self.assertIn(f"VMAF_BIN={missing} does not name an executable file", stderr.getvalue())


class BuildHasCudaTest(unittest.TestCase):
    def has_cuda(self, options: str | None) -> bool | None:
        with build_dir_with(options) as build:
            has_cuda: bool | None = load_default_run().build_has_cuda(Path(build))
        return has_cuda

    def test_build_configured_without_cuda_is_recognised(self) -> None:
        options = json.dumps(
            [{"name": "enable_hip", "value": True}, {"name": "enable_cuda", "value": False}]
        )
        self.assertIs(self.has_cuda(options), False)

    def test_build_configured_with_cuda_is_recognised(self) -> None:
        self.assertIs(self.has_cuda(json.dumps([{"name": "enable_cuda", "value": True}])), True)

    def test_a_build_that_does_not_say_is_not_skipped(self) -> None:
        # No record, an unreadable one, or one without the option: the run
        # goes on and the CLI's own refusal decides.
        self.assertIsNone(self.has_cuda(None))
        self.assertIsNone(self.has_cuda("{not json"))
        self.assertIsNone(self.has_cuda(json.dumps({"enable_cuda": False})))
        self.assertIsNone(self.has_cuda(json.dumps([{"name": "enable_hip", "value": True}])))
        self.assertIsNone(self.has_cuda(json.dumps([{"name": "enable_cuda", "value": "auto"}])))

    def test_a_build_without_cuda_is_skipped_before_the_device_lock(self) -> None:
        """Skip CPU-only builds before constructing or launching the lock command."""
        module = load_default_run()
        options = json.dumps([{"name": "enable_cuda", "value": False}])
        with build_dir_with(options) as build:
            binary = Path(build) / "tools" / "vmaf"
            binary.parent.mkdir()
            binary.write_text("", encoding="utf-8")
            self.assertEqual(module.skip_reason(binary), "This build was configured without CUDA")
            with (
                mock.patch.object(module, "find_binary", return_value=binary),
                mock.patch.object(
                    module.subprocess,
                    "Popen",
                    side_effect=AssertionError("lock acquired before skip"),
                ) as popen,
                contextlib.redirect_stderr(io.StringIO()),
            ):
                self.assertEqual(module.main(), 77)
                popen.assert_not_called()
        self.assertEqual(module.skip_reason(None), module.vmaftest.MISSING_MESSAGE)
        # main() decides the skip ahead of the command that waits for the lock.
        source = DEFAULT_RUN.read_text(encoding="utf-8")
        main = source[source.index("def main() -> int:") :]
        self.assertLess(
            main.index("reason = skip_reason(binary)"), main.index("gate_command(binary)")
        )


def meson_timeout(test_name: str) -> int:
    """The `timeout :` that core/test/meson.build gives the test `test_name`."""
    block = re.search(
        rf"^test\('{re.escape(test_name)}',(?P<body>.*?)^\)",
        MESON_BUILD.read_text(encoding="utf-8"),
        re.MULTILINE | re.DOTALL,
    )
    if block is None:
        raise AssertionError(f"core/test/meson.build no longer registers {test_name}")
    timeout = re.search(r"\btimeout\s*:\s*(\d+)", block.group("body"))
    if timeout is None:
        raise AssertionError(f"{test_name} has no timeout in core/test/meson.build")
    return int(timeout.group(1))


class DefaultRunTimeoutTest(unittest.TestCase):
    """main() of the default run, with the device-free steps replaced by stubs."""

    def run_main(
        self, outcome: subprocess.CompletedProcess[str] | BaseException
    ) -> tuple[ModuleType, int, str, mock.MagicMock]:
        """Run main() with `outcome` as the result, or the exception, of the gate run."""
        module = load_default_run()
        stderr = io.StringIO()
        binary = Path("build") / "tools" / "vmaf"
        with contextlib.ExitStack() as stack:
            stack.enter_context(mock.patch.object(module, "find_binary", return_value=binary))
            stack.enter_context(mock.patch.object(module, "skip_reason", return_value=None))
            gate = stack.enter_context(mock.patch.object(module.subprocess, "Popen"))
            proc = gate.return_value.__enter__.return_value
            if isinstance(outcome, BaseException):
                proc.communicate.side_effect = [outcome, ("partial stdout", "partial stderr")]
            else:
                proc.returncode = outcome.returncode
                proc.communicate.return_value = (outcome.stdout, outcome.stderr)
            stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
            stack.enter_context(contextlib.redirect_stderr(stderr))
            rc = module.main()
        return module, rc, stderr.getvalue(), gate

    def test_a_clean_run_passes_and_flock_execs_the_gate(self) -> None:
        """Retain flock exec semantics and bound communicate on successful runs."""
        cells = "motion         cpu    ↔ cuda  PASS\ncambi          cpu    ↔ cuda  PASS\n"
        module, rc, _, gate = self.run_main(subprocess.CompletedProcess([], 0, cells, ""))
        self.assertEqual(rc, 0)
        # Without --no-fork the timeout kills flock and the forked gate keeps the lock.
        self.assertEqual(gate.call_args.args[0][:2], ["flock", "--no-fork"])
        proc = gate.return_value.__enter__.return_value
        proc.communicate.assert_called_once_with(timeout=module.SCRIPT_TIMEOUT_S)

    def test_a_hung_gate_returns_124_and_names_the_bound(self) -> None:
        """Interrupt a hung gate, retaining diagnostics and the named timeout."""
        module, rc, stderr, gate = self.run_main(subprocess.TimeoutExpired(["flock"], 105))
        self.assertEqual(rc, 124)
        self.assertIn(f"timed out after {module.SCRIPT_TIMEOUT_S}s (SCRIPT_TIMEOUT_S)", stderr)
        self.assertIn("partial stderr", stderr)
        proc = gate.return_value.__enter__.return_value
        proc.send_signal.assert_called_once_with(signal.SIGINT)
        self.assertEqual(proc.communicate.call_args.kwargs["timeout"], module.INTERRUPT_GRACE_S)

    def test_the_bound_is_under_the_meson_timeout(self) -> None:
        """Keep the command timeout and both kill graces below Meson termination."""
        # At or over the Meson timeout, Meson kills the test first and the
        # message above never prints.
        module = load_default_run()
        bound = module.SCRIPT_TIMEOUT_S + module.INTERRUPT_GRACE_S + module.KILL_GRACE_S
        self.assertLess(bound, meson_timeout("test_cuda_parity_gate_default_run"))

    def test_partial_output_is_decoded_or_empty(self) -> None:
        """TimeoutExpired carries bytes or None; undecodable bytes are replaced, not raised."""
        module = load_default_run()
        self.assertEqual(module.as_text(b"partial \xff"), "partial �")
        self.assertEqual(module.as_text(None), "")
        self.assertEqual(module.as_text(""), "")
        self.assertEqual(module.as_text("already text"), "already text")


@unittest.skipUnless(os.name == "posix" and shutil.which("flock"), "POSIX flock is unavailable")
class DefaultRunProcessCleanupTest(unittest.TestCase):
    """Exercise the real gate supervisor with a fake vmaf, without a GPU."""

    def test_timeout_reaps_vmaf_before_releasing_the_lock(self) -> None:
        """SIGINT must reap the safe_subprocess child before the GPU lock is free."""
        import fcntl  # noqa: PLC0415 -- unavailable on Windows; imported only after POSIX skip.

        module = load_default_run()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            pid_file, lock = root / "pid", root / "lock"
            fake = root / "vmaf"
            fake.write_text(
                f"#!{sys.executable}\nimport os, time\nfrom pathlib import Path\n"
                f"Path({str(pid_file)!r}).write_text(str(os.getpid()))\n"
                "print('fake vmaf started', flush=True)\ntime.sleep(600)\n",
                encoding="utf-8",
            )
            fake.chmod(0o755)
            command = module.gate_command(fake, lock_path=lock)
            command += ["--features", "motion", "--workdir", str(root / "work")]
            stdout, stderr = io.StringIO(), io.StringIO()
            try:
                with (
                    mock.patch.object(module, "find_binary", return_value=fake),
                    mock.patch.object(module, "skip_reason", return_value=None),
                    mock.patch.object(module, "gate_command", return_value=command),
                    mock.patch.object(module, "SCRIPT_TIMEOUT_S", 3),
                    mock.patch.object(module, "INTERRUPT_GRACE_S", 5),
                    contextlib.redirect_stdout(stdout),
                    contextlib.redirect_stderr(stderr),
                ):
                    self.assertEqual(module.main(), 124)
                self.assertIn("timed out after 3s", stderr.getvalue())
                self.assertTrue(pid_file.is_file(), stderr.getvalue())
                pid = int(pid_file.read_text())
                self.assertTrue(wait_until_gone(pid), f"vmaf {pid} survived gate timeout")
                with lock.open("a") as descriptor:
                    fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
            finally:
                if pid_file.is_file():
                    with contextlib.suppress(ProcessLookupError):
                        os.kill(int(pid_file.read_text()), signal.SIGKILL)

    def test_interrupt_ignoring_gate_is_killed_and_partial_output_is_printed(self) -> None:
        """Bound fallback even for a gate ignoring SIGINT, with a watchdog for mutants."""
        module = load_default_run()
        popen = subprocess.Popen
        watchdogs = []

        def start(*args: Any, **kwargs: Any) -> subprocess.Popen[str]:
            """Start a real gate and arrange bounded cleanup if the fallback is removed."""
            proc: subprocess.Popen[str] = popen(*args, **kwargs)
            watchdog = threading.Timer(3, proc.kill)
            watchdog.start()
            watchdogs.append(watchdog)
            return proc

        script = (
            "import signal, sys, time; signal.signal(signal.SIGINT, signal.SIG_IGN); "
            "print('partial out', flush=True); "
            "print('partial err', file=sys.stderr, flush=True); time.sleep(600)"
        )
        stdout, stderr = io.StringIO(), io.StringIO()
        started = time.monotonic()
        try:
            with (
                mock.patch.object(module.subprocess, "Popen", side_effect=start),
                mock.patch.object(module, "SCRIPT_TIMEOUT_S", 0.5),
                mock.patch.object(module, "INTERRUPT_GRACE_S", 0.5),
                contextlib.redirect_stdout(stdout),
                contextlib.redirect_stderr(stderr),
            ):
                result = module.run_gate([sys.executable, "-c", script])
            self.assertIsNone(result)
            self.assertLess(time.monotonic() - started, 2.5)
            self.assertIn("partial out", stdout.getvalue())
            self.assertIn("partial err", stderr.getvalue())
        finally:
            for watchdog in watchdogs:
                watchdog.cancel()
                watchdog.join()

    def test_a_descendant_holding_the_pipes_does_not_hang_the_kill(self) -> None:
        """After SIGKILL, wait at most KILL_GRACE_S for pipes a descendant kept open."""
        module = load_default_run()
        with tempfile.TemporaryDirectory() as directory:
            pid_file = Path(directory) / "pid"
            script = (
                "import signal, subprocess, sys, time; "
                "signal.signal(signal.SIGINT, signal.SIG_IGN); "
                "holder = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(600)'],"
                " start_new_session=True); "
                f"open({str(pid_file)!r}, 'w').write(str(holder.pid)); "
                "print('partial out', flush=True); time.sleep(600)"
            )
            # Without the bound the test would hang; the watchdog closes the pipes at 4 s,
            # so a removed bound fails the elapsed-time assertion instead.
            watchdog = threading.Timer(4, kill_pid_in, args=(pid_file,))
            stdout, stderr = io.StringIO(), io.StringIO()
            started = time.monotonic()
            watchdog.start()
            try:
                with (
                    mock.patch.object(module, "SCRIPT_TIMEOUT_S", 0.5),
                    mock.patch.object(module, "INTERRUPT_GRACE_S", 0.5),
                    mock.patch.object(module, "KILL_GRACE_S", 0.5),
                    contextlib.redirect_stdout(stdout),
                    contextlib.redirect_stderr(stderr),
                ):
                    result = module.run_gate([sys.executable, "-c", script])
                self.assertIsNone(result)
                self.assertLess(time.monotonic() - started, 3.5)
                self.assertIn("partial out", stdout.getvalue())
                self.assertIn("still held the output pipes 0.5s after the kill", stderr.getvalue())
            finally:
                watchdog.cancel()
                watchdog.join()
                kill_pid_in(pid_file)


def kill_pid_in(pid_file: Path) -> None:
    """SIGKILL the process whose PID `pid_file` holds, if it was written and still runs."""
    with contextlib.suppress(OSError, ValueError):
        os.kill(int(pid_file.read_text(encoding="utf-8")), signal.SIGKILL)


def wait_until_gone(pid: int) -> bool:
    """Poll for at most five seconds until the child PID has been reaped."""
    for _ in range(100):
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return True
        time.sleep(0.05)
    return False


if __name__ == "__main__":
    unittest.main()
