# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The backend probe of the tests never starts a ``vmaf`` found on ``PATH``.

``T-VMAFTUNE-TESTS-PROBE-PATH-VMAF-2026-10-05``: the probe defaults to the
bare name ``vmaf``; the suite-wide fixture in ``conftest.py`` keeps it off the
host's binary. Positive: a bare name sees a CPU-only build. Negative: a
``vmaf`` placed on ``PATH`` is never run. Boundary: an explicit path, or a
runner, still reaches the real probe, and a runner does so on a host with no
``vmaf`` on ``PATH`` (``T-VMAFTUNE-PROBE-RUNNER-READS-PATH-2026-10-06``).
"""

from __future__ import annotations

import json
import os
import stat
from pathlib import Path

import pytest

from vmaftune import score_backend

_CUDA_REPORT = {
    "backends": [
        {"name": "cpu", "compiled": True, "usable": True},
        {"name": "cuda", "compiled": True, "usable": True},
    ]
}


def _fake_vmaf(directory: Path, marker: Path) -> Path:
    """A ``vmaf`` that records its start in ``marker`` and reports CUDA usable."""
    script = directory / "vmaf"
    script.write_text(
        f"#!/bin/sh\necho started >> {marker}\ncat <<'EOF'\n{json.dumps(_CUDA_REPORT)}\nEOF\n"
    )
    script.chmod(script.stat().st_mode | stat.S_IXUSR)
    return script


def test_bare_vmaf_name_sees_a_cpu_only_build() -> None:
    assert score_backend.detect_available_backends() == ["cpu"]


def test_vmaf_on_path_is_never_started(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    marker = tmp_path / "started"
    _fake_vmaf(tmp_path, marker)
    monkeypatch.setenv("PATH", f"{tmp_path}{os.pathsep}{os.environ.get('PATH', '')}")

    assert score_backend.detect_available_backends() == ["cpu"]
    assert not marker.exists(), "the probe started the vmaf found on PATH"


def test_explicit_path_still_runs_the_probe(tmp_path: Path) -> None:
    marker = tmp_path / "started"
    script = _fake_vmaf(tmp_path, marker)

    assert score_backend.detect_available_backends(vmaf_bin=str(script)) == ["cpu", "cuda"]
    assert marker.exists()


def test_runner_still_reaches_the_probe(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    class _Done:
        returncode = 0
        stdout = json.dumps(_CUDA_REPORT)

    calls: list[object] = []

    def runner(argv: object, **_kwargs: object) -> _Done:
        calls.append(argv)
        return _Done()

    # The hosted runner has no vmaf on PATH; this host may. The runner's
    # answer must not depend on it.
    monkeypatch.setenv("PATH", str(tmp_path))
    assert score_backend.detect_available_backends(runner=runner) == ["cpu", "cuda"]
    assert calls == [["vmaf", "--list-backends"]]
