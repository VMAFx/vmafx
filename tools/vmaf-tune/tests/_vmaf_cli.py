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
from collections.abc import Callable, Sequence
from pathlib import Path

import pytest

# isort: split
# tests/conftest.py puts ../src and the repository root on sys.path before this loads.
from scripts.lib import safe_subprocess, vmaftest

# isort: split
from vmaftune.codec_adapters._ffmpeg_listing import encoder_listed

REPO_ROOT = Path(__file__).resolve().parents[3]


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


def run_absolute_tool(
    argv: Sequence[str],
    *,
    capture_output: bool = False,
    text: bool = False,
    errors: str = "strict",
    check: bool = False,
    timeout: float = safe_subprocess.DEFAULT_TIMEOUT_SECONDS,
) -> safe_subprocess.CommandResult:
    """Run ``argv`` through the repository's bounded runner with ``subprocess.run`` keywords.

    ``argv[0]`` must be an absolute path and is the only allowlisted executable, so a
    bare tool name fails here instead of resolving through ``PATH``. ``timeout`` is in
    seconds; the runner terminates the whole process group when it expires and raises
    ``safe_subprocess.CommandTimedOut``. The keyword names match ``subprocess.run`` so
    the function can stand in for it as a ``runner`` / ``encoder_runner`` hook.
    """
    executable = str(argv[0])
    if not Path(executable).is_absolute():
        raise ValueError(f"tool path must be absolute: {executable}")
    return safe_subprocess.run(
        list(argv),
        allowed_executables=[executable],
        capture_output=capture_output,
        text=text,
        errors=errors,
        check=check,
        timeout_seconds=timeout,
    )


def ffmpeg_reports_encoder(encoder: str) -> bool:
    """Return True iff the local ffmpeg binary advertises ``encoder``."""
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg is None or not Path(ffmpeg).is_absolute():
        return False
    try:
        result = run_absolute_tool(
            [ffmpeg, "-hide_banner", "-encoders"],
            capture_output=True,
            text=True,
            errors="replace",
            timeout=10,
        )
    except (
        OSError,
        safe_subprocess.CommandValidationError,
        safe_subprocess.CommandOutputLimitExceeded,
    ):
        return False
    return encoder_listed(str(result.stdout), encoder)
