#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Verify default cross-backend parity gate run completes for CPU vs CUDA.

Checks that scripts/ci/cross_backend_parity_gate.py runs without error when
comparing default features on CPU vs CUDA (e.g. on an RTX 4090).
"""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "ci" / "cross_backend_parity_gate.py"
REF = ROOT / "python" / "test" / "resource" / "yuv" / "src01_hrc00_576x324.yuv"
DIS = ROOT / "python" / "test" / "resource" / "yuv" / "src01_hrc01_576x324.yuv"


def main() -> int:
    vmaf_bin = os.environ.get("VMAF_BUILD_DIR")
    binary = Path(vmaf_bin) / "tools" / "vmaf" if vmaf_bin else ROOT / "build" / "tools" / "vmaf"

    if not binary.is_file():
        binary = Path.cwd() / "tools" / "vmaf"
        if not binary.is_file():
            sys.stderr.write(f"vmaf binary not found at {binary}\n")
            return 77

    if not REF.is_file() or not DIS.is_file():
        sys.stderr.write("Reference / distorted YUV fixtures not found\n")
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

    proc = subprocess.run(cmd, capture_output=True, text=True, check=False)  # noqa: S603
    if proc.returncode != 0:
        combined = proc.stdout + proc.stderr
        if "No CUDA device" in combined or "cudaErrorNoDevice" in combined:
            sys.stderr.write("No CUDA device available; skipping (77)\n")
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
