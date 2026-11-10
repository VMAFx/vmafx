# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Shared lookups of the VMAFx C sources and ``vmaf`` CLI for the vmaf-tune tests.

The source-pinning tests read the CLI sources under ``core/tools/`` and
the integration tests run the fork's ``vmaf`` binary. Both lookups live
here so a rename (``vmaf.c`` to ``vmaf.cpp``, ``libvmaf/`` to ``core/``)
fails every caller at once instead of turning some of them into silent
skips. The binary itself comes from ``scripts/lib/vmaftest.py``, the one
resolver every Python suite uses: ``VMAF_BIN``, ``VMAF_BIN_FOR_TESTS``,
then the repository's build directories, never ``PATH``.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
from collections.abc import Callable
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[3]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

# Imported after the sys.path line above, which makes it importable.
from scripts.lib import vmaftest


def repo_source(relative: str) -> str:
    """Return the text of ``relative`` under the repository root.

    Skips only when the tests run outside the VMAFx repository (no
    ``core/meson.build``); inside it a missing file is a failure, so a
    renamed source cannot hide behind a skip.
    """
    if not (REPO_ROOT / "core" / "meson.build").is_file():
        pytest.skip("not inside the VMAFx repository: core/meson.build is absent")
    return (REPO_ROOT / relative).read_text(encoding="utf-8")


def binary_supports_backend_flag(path: Path) -> bool:
    """True iff the binary advertises ``--backend`` in its help output.

    A ``vmaf`` that does not is not this fork's CLI (an upstream release,
    which even exits 0 on the unknown option), so it cannot stand in for
    the binary under test.
    """
    try:
        result = subprocess.run([str(path), "--help"], capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        return False
    return "--backend" in (result.stdout + result.stderr)


def vmaf_under_test() -> Path:
    """The ``vmaf`` CLI under test; skips the test with the resolver's message when there is none."""
    binary = vmaftest.find()
    if binary is None:
        pytest.skip(vmaftest.MISSING_MESSAGE)
    return binary


def fork_vmaf_under_test(
    supports: Callable[[Path], bool] = binary_supports_backend_flag,
) -> Path:
    """:func:`vmaf_under_test`, failing the test when it is not this fork's CLI.

    The binary under test is the only candidate: one that does not pass
    ``supports`` is a misconfigured ``VMAF_BIN`` or build, not a reason to
    look elsewhere.
    """
    binary = vmaf_under_test()
    if not supports(binary):
        pytest.fail(
            f"the vmaf binary under test ({binary}) does not advertise --backend, so it is "
            "not this fork's CLI; point VMAF_BIN at a VMAFx build"
        )
    return binary


# An `ffmpeg -encoders` row is "<flags> <encoder name> <description>"; a row with fewer
# fields names no encoder.
ENCODER_ROW_MIN_FIELDS = 2


def ffmpeg_reports_encoder(encoder: str) -> bool:
    """Return True iff the local ffmpeg binary advertises ``encoder``."""
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg is None:
        return False
    try:
        result = subprocess.run(
            [ffmpeg, "-hide_banner", "-encoders"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=10,
            check=False,
        )
        out = result.stdout.decode("utf-8", errors="replace")
        for line in out.splitlines():
            parts = line.split()
            if len(parts) >= ENCODER_ROW_MIN_FIELDS and parts[1] == encoder:
                return True
    except (OSError, subprocess.SubprocessError):
        return False
    return False
