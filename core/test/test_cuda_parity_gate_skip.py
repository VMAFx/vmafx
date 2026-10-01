#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""test_cuda_parity_gate_default_run must skip where CUDA cannot run at all.

The default-run test compares CPU with CUDA on a device. It is registered for
every build, so on a libvmaf built without CUDA (a HIP-only or SYCL-only build
dir) the CLI refuses `--backend cuda` (ADR-0498) and the test used to report
that refusal as a failure of the parity gate. This device-free test pins the
skip decision to the message the CLI actually prints.
"""

from __future__ import annotations

import importlib.util
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_RUN = ROOT / "core" / "test" / "test_cuda_parity_gate_default_run.py"
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


if __name__ == "__main__":
    unittest.main()
