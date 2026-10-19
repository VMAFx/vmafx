# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Suite-wide pytest setup for the vmaf-tune tests."""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "src"))

from vmaftune import (
    score_backend,
)


@pytest.fixture(autouse=True)
def _private_working_directory(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """Run every test in its own temporary working directory.

    Subprocess fakes write to the last argv element; for the encoder-version
    probe (``ffmpeg -version``) that left a ``-version`` file in the checkout,
    and relative defaults such as ``.workingdir/cache/vmafx-tune/encodes``
    landed there too. Tests reach repository files through ``__file__``.
    """
    monkeypatch.chdir(tmp_path)


@pytest.fixture(autouse=True)
def _backend_probe_never_reads_path(monkeypatch: pytest.MonkeyPatch) -> None:
    """Keep the backend probe off the host's ``vmaf``.

    ``select_backend()`` / ``detect_available_backends()`` probe
    ``vmaf --list-backends`` and default to the bare name ``vmaf``, which the
    OS resolves through ``PATH``: about forty CLI tests started whatever
    ``vmaf`` the host had, so their path depended on it
    (``T-VMAFTUNE-TESTS-PROBE-PATH-VMAF-2026-10-05``). A probe with neither a
    runner nor a path now sees a CPU-only build. A test that exercises the
    probe passes a runner or the path of a ``vmaf`` of its own, which runs
    for real.
    """
    real = score_backend.backend_report

    def _hermetic(vmaf_bin: str = "vmaf", runner: object | None = None):
        if runner is None and "/" not in vmaf_bin:
            return {"cpu": {"name": "cpu", "compiled": True, "usable": True}}
        return real(vmaf_bin, runner)

    monkeypatch.setattr(score_backend, "backend_report", _hermetic)
