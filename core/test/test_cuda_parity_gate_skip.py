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
import re
import subprocess
import tempfile
import unittest
from pathlib import Path
from types import ModuleType
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


def load_default_run():
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


def build_dir_with(options: str | None) -> tempfile.TemporaryDirectory:
    """A build directory whose Meson option record holds `options`, or none."""
    build = tempfile.TemporaryDirectory()
    if options is not None:
        info = Path(build.name) / "meson-info"
        info.mkdir()
        (info / "intro-buildoptions.json").write_text(options, encoding="utf-8")
    return build


class BuildHasCudaTest(unittest.TestCase):
    def has_cuda(self, options: str | None) -> bool | None:
        with build_dir_with(options) as build:
            return load_default_run().build_has_cuda(Path(build))

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
        module = load_default_run()
        options = json.dumps([{"name": "enable_cuda", "value": False}])
        with build_dir_with(options) as build:
            binary = Path(build) / "tools" / "vmaf"
            binary.parent.mkdir()
            binary.write_text("", encoding="utf-8")
            self.assertEqual(module.skip_reason(binary), "This build was configured without CUDA")
        self.assertEqual(module.skip_reason(None), "vmaf binary not found")
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

    def run_main(self, outcome: object) -> tuple[ModuleType, int, str, mock.MagicMock]:
        """Run main() with `outcome` as the result, or the exception, of the gate run."""
        module = load_default_run()
        stderr = io.StringIO()
        binary = Path("build") / "tools" / "vmaf"
        with contextlib.ExitStack() as stack:
            stack.enter_context(mock.patch.object(module, "find_binary", return_value=binary))
            stack.enter_context(mock.patch.object(module, "skip_reason", return_value=None))
            gate = stack.enter_context(mock.patch.object(module.subprocess, "run"))
            if isinstance(outcome, BaseException):
                gate.side_effect = outcome
            else:
                gate.return_value = outcome
            stack.enter_context(contextlib.redirect_stderr(stderr))
            rc = module.main()
        return module, rc, stderr.getvalue(), gate

    def test_a_clean_run_passes_and_flock_execs_the_gate(self) -> None:
        cells = "motion         cpu    ↔ cuda  PASS\ncambi          cpu    ↔ cuda  PASS\n"
        module, rc, _, gate = self.run_main(subprocess.CompletedProcess([], 0, cells, ""))
        self.assertEqual(rc, 0)
        # Without --no-fork the timeout kills flock and the forked gate keeps the lock.
        self.assertEqual(gate.call_args.args[0][:2], ["flock", "--no-fork"])
        self.assertEqual(gate.call_args.kwargs.get("timeout"), module.SCRIPT_TIMEOUT_S)

    def test_a_hung_gate_returns_124_and_names_the_bound(self) -> None:
        module, rc, stderr, _ = self.run_main(subprocess.TimeoutExpired(["flock"], 110))
        self.assertEqual(rc, 124)
        self.assertIn(f"timed out after {module.SCRIPT_TIMEOUT_S}s (SCRIPT_TIMEOUT_S)", stderr)

    def test_the_bound_is_under_the_meson_timeout(self) -> None:
        # At or over the Meson timeout, Meson kills the test first and the
        # message above never prints.
        bound = load_default_run().SCRIPT_TIMEOUT_S
        self.assertLess(bound, meson_timeout("test_cuda_parity_gate_default_run"))


if __name__ == "__main__":
    unittest.main()
