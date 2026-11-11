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
Exit status: 0 for success; the command's status on ordinary failure; 2 for usage;
124 for timeout; 125 for captured-output limit; 126 for validation or launch failure.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.lib.safe_subprocess import (
    CommandOutputLimitExceeded,
    CommandTimedOut,
    CommandValidationError,
)
from scripts.lib.safe_subprocess import run as run_command

# Bound = 10x the slowest measured run, rounded up: the scratch_check.cpp AOT
# compile for 19 device targets took 10.72 s.
CC_TIMEOUT_S = 120
# Budget: 1 MiB of diagnostics per AOT target plus 1 MiB for the driver (19 targets).
# This 20 MiB ceiling allows verbose failures without retaining an unbounded compiler log.
MAX_COMPILE_OUTPUT_BYTES = 20 * 1_048_576


def _print_retained_stdout(output: str | bytes | None) -> None:
    """Print diagnostic output retained before a timeout or output overflow."""
    if output:
        sys.stdout.write(output if isinstance(output, str) else output.decode("utf-8", "replace"))


def main(argv: list[str]) -> int:
    """Run a command with combined output and a timeout, returning its exit code."""
    if argv[:1] != ["--"] or len(argv) == 1:
        sys.stderr.write("usage: run_captured.py -- COMMAND [ARGS...]\n")
        return 2
    try:
        result = run_command(
            argv[1:],
            allowed_executables=[argv[1]],
            capture_output=True,
            stderr_to_stdout=True,
            text=True,
            errors="replace",
            check=False,
            timeout_seconds=CC_TIMEOUT_S,
            max_output_bytes=MAX_COMPILE_OUTPUT_BYTES,
        )
    except CommandTimedOut as exc:
        sys.stderr.write(
            f"run_captured: command timed out after {CC_TIMEOUT_S}s (CC_TIMEOUT_S): {argv[1:]}\n"
        )
        _print_retained_stdout(exc.stdout)
        return 124
    except CommandOutputLimitExceeded as exc:
        sys.stderr.write(
            "run_captured: command exceeded "
            f"{MAX_COMPILE_OUTPUT_BYTES} captured bytes (MAX_COMPILE_OUTPUT_BYTES): {argv[1:]}\n"
        )
        _print_retained_stdout(exc.stdout)
        return 125
    except (CommandValidationError, OSError) as exc:
        reason = str(exc).replace("\r", "\\r").replace("\n", "\\n")
        sys.stderr.write(f"run_captured: {reason}\n")
        return 126
    if result.returncode != 0:
        sys.stdout.write(result.stdout)
    return result.returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
