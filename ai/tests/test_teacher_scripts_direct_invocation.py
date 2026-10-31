# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The scripts that resolve a teacher model start as ``python ai/scripts/<name>.py``.

``ai.data.scores`` lives under the repository root, which a directly started
script does not have on ``sys.path``; the mini-retrain pipeline starts them
that way and failed with ``No module named 'ai'``.
"""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path
from typing import Any, Callable, TypeVar, cast

import pytest

F = TypeVar("F", bound=Callable[..., Any])


def _parametrize(*args: Any, **kwargs: Any) -> Callable[[F], F]:
    return cast(Callable[[F], F], pytest.mark.parametrize(*args, **kwargs))


_REPO = Path(__file__).resolve().parents[2]
_SCRIPTS = (
    "export_ensemble_v2_seeds.py",
    "train_fr_regressor.py",
    "train_fr_regressor_v2.py",
    "train_fr_regressor_v2_ensemble.py",
    "train_fr_regressor_v3.py",
)


def _start(script: Path, tmp_path: Path) -> subprocess.CompletedProcess[str]:
    env = {**os.environ, "PYTHONPATH": str(_REPO / "ai" / "src")}
    return subprocess.run(
        [sys.executable, str(script), "--help"],
        capture_output=True,
        text=True,
        cwd=tmp_path,
        env=env,
        check=False,
        timeout=120,
    )


@_parametrize("name", _SCRIPTS)
def test_script_starts_outside_the_repository(name: str, tmp_path: Path) -> None:
    """Each script prints its help when started by path from another folder."""
    done = _start(_REPO / "ai" / "scripts" / name, tmp_path)
    assert done.returncode == 0, done.stderr[-800:]


def test_a_script_without_the_repository_root_fails(tmp_path: Path) -> None:
    """The check itself: a copy that cannot reach ``ai`` exits non-zero."""
    script = tmp_path / "lost.py"
    script.write_text("from ai.data.scores import resolve_teacher_model\n", encoding="utf-8")
    done = _start(script, tmp_path)
    assert done.returncode != 0
    assert "No module named 'ai'" in done.stderr
