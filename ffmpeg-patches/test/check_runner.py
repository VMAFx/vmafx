#!/usr/bin/env python3
# Copyright 2026 luoye-cpu
# SPDX-License-Identifier: EUPL-1.2
"""Shared process helper for the FFmpeg functional checkers."""

from __future__ import annotations

import argparse
import os
import subprocess
from typing import Sequence

TIMEOUT_SECONDS = 600


def run(args: argparse.Namespace, cmd: Sequence[str]) -> subprocess.CompletedProcess[str]:
    """Run the binary under test, turning launch and timeout failures into rows.

    The callers print a FAIL row from ``returncode`` and ``stderr``; letting
    ``OSError`` or ``TimeoutExpired`` escape would replace the row and the
    summary with a traceback, which is exactly what a CI summary must not do.
    """
    env = dict(os.environ)
    libdir = getattr(args, "libdir", "")
    if libdir:
        env["LD_LIBRARY_PATH"] = libdir + ":" + env.get("LD_LIBRARY_PATH", "")
    try:
        return subprocess.run(  # noqa: S603 -- binary under test, argument list, no shell
            list(cmd),
            env=env,
            capture_output=True,
            text=True,
            timeout=TIMEOUT_SECONDS,
            check=False,
        )
    except OSError as exc:
        return subprocess.CompletedProcess(
            list(cmd),
            127,
            "",
            f"{cmd[0]}: {exc.strerror or exc}\n",
        )
    except subprocess.TimeoutExpired as exc:
        stdout = exc.stdout if isinstance(exc.stdout, str) else ""
        return subprocess.CompletedProcess(
            list(cmd),
            124,
            stdout,
            f"{cmd[0]} timed out after {TIMEOUT_SECONDS}s\n",
        )
