# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for the direct-invocation bootstrap used by AI scripts."""

from __future__ import annotations

import importlib
import sys
from pathlib import Path

import _script_bootstrap
from _script_bootstrap import bootstrap_ai_script


def test_bootstrap_returns_expected_repo_paths(monkeypatch) -> None:
    script = Path(__file__).resolve().parents[1] / "scripts" / "dummy.py"
    monkeypatch.setattr(sys, "path", [])

    paths = bootstrap_ai_script(
        script,
        include_repo_root=True,
        include_ai_scripts=True,
        include_vmaf_tune_src=True,
    )

    repo_root = Path(__file__).resolve().parents[2]
    assert paths.script_path == script
    assert paths.repo_root == repo_root
    assert paths.ai_dir == repo_root / "ai"
    assert paths.ai_src == repo_root / "ai" / "src"
    assert paths.ai_scripts == repo_root / "ai" / "scripts"
    assert paths.vmaf_tune_src == repo_root / "tools" / "vmaf-tune" / "src"
    assert str(paths.repo_root) in sys.path
    assert str(paths.ai_src) in sys.path
    assert str(paths.ai_scripts) in sys.path
    assert str(paths.vmaf_tune_src) in sys.path


def test_bootstrap_does_not_duplicate_paths(monkeypatch) -> None:
    script = Path(__file__).resolve().parents[1] / "scripts" / "dummy.py"
    repo_root = Path(__file__).resolve().parents[2]
    ai_src = repo_root / "ai" / "src"
    monkeypatch.setattr(sys, "path", [str(ai_src)])

    bootstrap_ai_script(script)
    bootstrap_ai_script(script)

    assert sys.path.count(str(ai_src)) == 1


def test_import_installs_all_static_import_roots(monkeypatch) -> None:
    scripts_dir = Path(_script_bootstrap.__file__).resolve().parent
    monkeypatch.setattr(sys, "path", [str(scripts_dir)])

    reloaded = importlib.reload(_script_bootstrap)

    paths = reloaded._IMPORT_PATHS
    assert paths.script_path == scripts_dir / "_script_bootstrap.py"
    assert sys.path[:4] == [
        str(paths.vmaf_tune_src),
        str(paths.ai_scripts),
        str(paths.ai_src),
        str(paths.repo_root),
    ]
