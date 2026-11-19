#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Symbolic-link capability probe for fixtures under ``scripts/ci/tests``.

Windows volumes or filesystem filter drivers that reject symbolic links raise
``OSError`` with ``WinError 87``, and Windows hosts without the symbolic-link
privilege raise ``OSError`` with ``WinError 1314``. Under HISS-21 a platform
gate that cannot run skips with a stated reason.

Rule: call :func:`skip_without_symlinks` inside the function that creates the
symbolic link, before its first link creation, passing the directory that
receives the link.
"""

from __future__ import annotations

import secrets
import unittest
from pathlib import Path


def symlink_probe_error(directory: Path) -> OSError | None:
    """Return the ``OSError`` from creating a probe link in ``directory``, or ``None``."""
    probe = directory / f".symlink-probe-{secrets.token_hex(4)}"
    try:
        probe.symlink_to("symlink-probe-target")
    except OSError as error:
        return error
    probe.unlink()
    return None


def skip_without_symlinks(directory: Path) -> None:
    """Raise :exc:`unittest.SkipTest` naming the probe error if ``directory`` rejects links."""
    error = symlink_probe_error(directory)
    if error is not None:
        raise unittest.SkipTest(f"cannot create a symbolic link in {directory}: {error}")
