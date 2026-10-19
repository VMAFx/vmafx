# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

"""Run a command and print its output only when it fails.

The SYCL self-test (scratch_check.cpp) holds a kernel, VmafSyclScratchProbeSpill, whose job
is to spill registers so the audit can prove it detects a spill. The device compiler reports
that spill as a warning on every AOT target (ocloc: "RetryManager ... spilled around 128"),
which is the probe working, not a defect, and which a build that treats warnings as errors
(ADR-2170) must not carry. The build runs that one compile through this wrapper: its output is
kept for a failed build and dropped for a successful one.

Usage: run_captured.py -- COMMAND [ARGS...]
"""

from __future__ import annotations

import subprocess
import sys


def main(argv: list[str]) -> int:
    if argv[:1] != ["--"] or len(argv) == 1:
        sys.stderr.write("usage: run_captured.py -- COMMAND [ARGS...]\n")
        return 2
    result = subprocess.run(  # noqa: S603 -- the command is the build's own compiler line.
        argv[1:],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
        check=False,
    )
    if result.returncode != 0:
        sys.stdout.write(result.stdout)
    return result.returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
