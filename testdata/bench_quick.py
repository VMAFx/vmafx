#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

"""Quick benchmark: three bounded runs per resolution, reporting best fps."""

import os
import subprocess
import sys
import time
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from testdata._command import run_command
else:
    try:
        from testdata._command import run_command
    except ModuleNotFoundError:
        from _command import run_command

FRAMES = 48.0
GIT_TIMEOUT_SECONDS = 10.0
RUN_TIMEOUT_SECONDS = 300.0
RUNS = 3
RESOLUTIONS = ("576x324", "640x480", "1280x720", "1920x1080", "3840x2160")


def _repo_root() -> Path:
    fallback = Path(__file__).resolve().parent.parent
    try:
        result = run_command(
            ["git", "rev-parse", "--show-toplevel"],
            capture_output=True,
            text=True,
            check=True,
            timeout=GIT_TIMEOUT_SECONDS,
        )
    except (OSError, subprocess.SubprocessError):
        return fallback
    output = result.stdout.strip()
    return Path(output) if output else fallback


def _diagnostic(value: object) -> str:
    for field in ("stderr", "stdout"):
        output = getattr(value, field, None)
        if isinstance(output, bytes):
            output = output.decode(errors="replace")
        if isinstance(output, str) and output.strip():
            return output.strip().splitlines()[-1]
    return str(value)


def _monotonic_seconds() -> float:
    return time.perf_counter()


def _command(vmaf_bin: str, fixture_root: Path, dimensions: str) -> list[str]:
    width, height = dimensions.split("x", maxsplit=1)
    return [
        vmaf_bin,
        "-r",
        str(fixture_root / f"ref_{dimensions}_48f.yuv"),
        "-d",
        str(fixture_root / f"dis_{dimensions}_48f.yuv"),
        "-w",
        width,
        "-h",
        height,
        "-p",
        "420",
        "-b",
        "8",
        "-m",
        "version=vmaf_v0.6.1",
        "--json",
        "-o",
        os.devnull,
        "--no_cuda",
    ]


def _benchmark_resolution(
    dimensions: str,
    *,
    fixture_root: Path,
    vmaf_bin: str,
    env: dict[str, str],
) -> bool:
    command = _command(vmaf_bin, fixture_root, dimensions)
    rates: list[float] = []
    for _ in range(RUNS):
        started = _monotonic_seconds()
        try:
            result = run_command(
                command,
                capture_output=True,
                text=True,
                env=env,
                timeout=RUN_TIMEOUT_SECONDS,
            )
        except (OSError, subprocess.SubprocessError) as error:
            print(f"{dimensions}: FAILED: {_diagnostic(error)}", file=sys.stderr)
            return False
        if result.returncode != 0:
            print(f"{dimensions}: FAILED: {_diagnostic(result)}", file=sys.stderr)
            return False
        rates.append(FRAMES / (_monotonic_seconds() - started))

    print(
        f"{dimensions}: best={max(rates):.1f} avg={sum(rates) / len(rates):.1f} fps  "
        f"({', '.join(f'{rate:.1f}' for rate in rates)})"
    )
    return True


def main() -> int:
    configured_root = os.environ.get("VMAF_TESTDATA")
    fixture_root = Path(configured_root) if configured_root else _repo_root() / "testdata"
    vmaf_bin = os.environ.get("VMAF_BIN", "/usr/local/bin/vmaf")
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = "/usr/local/lib"

    attempted = 0
    failed = False
    for dimensions in RESOLUTIONS:
        ref = fixture_root / f"ref_{dimensions}_48f.yuv"
        distorted = fixture_root / f"dis_{dimensions}_48f.yuv"
        if not ref.is_file() or not distorted.is_file():
            continue
        attempted += 1
        failed |= not _benchmark_resolution(
            dimensions,
            fixture_root=fixture_root,
            vmaf_bin=vmaf_bin,
            env=env,
        )

    if attempted == 0:
        print(f"No complete 48-frame fixture pairs found in {fixture_root}", file=sys.stderr)
        return 2
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
