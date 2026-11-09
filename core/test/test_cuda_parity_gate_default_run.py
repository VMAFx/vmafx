#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Verify default cross-backend parity gate run completes for CPU vs CUDA.

Checks that scripts/ci/cross_backend_parity_gate.py runs without error when
comparing default features on CPU vs CUDA (e.g. on an RTX 4090). A binary
built without CUDA, or a host without a CUDA device, has nothing to compare:
the test is skipped (exit 77), not failed.

A build directory configured without CUDA is skipped before the device lock is
taken: waiting for a device this build cannot use ran into the test's timeout
whenever another job held the lock.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "ci" / "cross_backend_parity_gate.py"
# Not measured: the parity gate needs a free GPU, which the measuring host did not have.
SCRIPT_TIMEOUT_S = 300
REF = ROOT / "python" / "test" / "resource" / "yuv" / "src01_hrc00_576x324.yuv"
DIS = ROOT / "python" / "test" / "resource" / "yuv" / "src01_hrc01_576x324.yuv"


# What the CLI prints when --backend cuda cannot run at all: no device, or a
# libvmaf built without CUDA (ADR-0498 refuses the silent CPU fallback).
NO_CUDA_MARKERS = (
    "No CUDA device",
    "cudaErrorNoDevice",
    "built without cuda support",
)


def cuda_unavailable(output: str) -> bool:
    """True when `output` says the run could not use CUDA at all."""
    return any(marker in output for marker in NO_CUDA_MARKERS)


def build_has_cuda(build_dir: Path) -> bool | None:
    """Whether Meson configured `build_dir` with CUDA; None when it does not say."""
    options = build_dir / "meson-info" / "intro-buildoptions.json"
    try:
        entries = json.loads(options.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    if not isinstance(entries, list):
        return None
    for entry in entries:
        if isinstance(entry, dict) and entry.get("name") == "enable_cuda":
            value = entry.get("value")
            return value if isinstance(value, bool) else None
    return None


def find_binary() -> Path | None:
    """The vmaf CLI of VMAF_BUILD_DIR, of `build/`, or of the working directory."""
    vmaf_bin = os.environ.get("VMAF_BUILD_DIR")
    binary = Path(vmaf_bin) / "tools" / "vmaf" if vmaf_bin else ROOT / "build" / "tools" / "vmaf"
    if binary.is_file():
        return binary
    binary = Path.cwd() / "tools" / "vmaf"
    return binary if binary.is_file() else None


def skip_reason(binary: Path | None) -> str | None:
    """Why there is nothing to compare, or None. Decided without the device."""
    if binary is None:
        return "vmaf binary not found"
    if build_has_cuda(binary.parent.parent) is False:
        return "This build was configured without CUDA"
    if not REF.is_file() or not DIS.is_file():
        return "Reference / distorted YUV fixtures not found"
    return None


def main() -> int:
    binary = find_binary()
    reason = skip_reason(binary)
    if reason is not None:
        sys.stderr.write(f"{reason}; skipping (77)\n")
        return 77

    cmd = [
        "flock",
        str(Path.home() / ".cache" / "vmafx-locks" / "cuda-4090.lock"),
        sys.executable,
        str(SCRIPT),
        "--vmaf-binary",
        str(binary),
        "--reference",
        str(REF),
        "--distorted",
        str(DIS),
        "--width",
        "576",
        "--height",
        "324",
        "--backends",
        "cpu",
        "cuda",
    ]

    try:
        proc = subprocess.run(  # noqa: S603
            cmd,
            capture_output=True,
            text=True,
            check=False,
            timeout=SCRIPT_TIMEOUT_S,
        )
    except subprocess.TimeoutExpired:
        sys.stderr.write(
            f"Default parity gate timed out after {SCRIPT_TIMEOUT_S}s (SCRIPT_TIMEOUT_S)\n"
        )
        return 124
    if proc.returncode != 0:
        if cuda_unavailable(proc.stdout + proc.stderr):
            sys.stderr.write("CUDA is not available to this binary; skipping (77)\n")
            return 77
        sys.stderr.write(
            f"Default parity gate failed:\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}\n"
        )
        return proc.returncode

    if (
        "motion         cpu    ↔ cuda" not in proc.stdout
        or "cambi          cpu    ↔ cuda" not in proc.stdout
    ):
        sys.stderr.write(f"Missing expected cells in output:\n{proc.stdout}\n")
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
