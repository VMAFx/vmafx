# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for the macOS bundle run script, on Linux with a fake `uname`."""

from __future__ import annotations

import json
import os
import shutil
import stat
import subprocess
import sys
from pathlib import Path

import pytest

_ROOT = Path(__file__).resolve().parents[3]
RUN_SH = _ROOT / "tools" / "rc1-tester" / "image" / "macos" / "run.sh"

pytestmark = pytest.mark.skipif(shutil.which("bash") is None, reason="needs bash")


def script(path: Path, body: str) -> None:
    path.write_text("#!/bin/sh\n" + body)
    path.chmod(path.stat().st_mode | stat.S_IEXEC)


def test_run_sh_refuses_other_platforms() -> None:
    result = subprocess.run(["sh", str(RUN_SH)], capture_output=True, text=True, check=False)
    assert result.returncode == 64 and "Apple silicon" in result.stderr


def test_run_sh_end_to_end_with_everything_skipped(tmp_path: Path) -> None:
    bundle = tmp_path / "b"
    (bundle / "runtime" / "bin").mkdir(parents=True)
    (bundle / "runtime" / "bin" / "python3").symlink_to(sys.executable)
    shutil.copytree(_ROOT / "tools" / "rc1-tester" / "src", bundle / "tester" / "src")
    shutil.copy(_ROOT / "tools" / "rc1-tester" / "vmaf-tester-report", bundle / "tester")
    (bundle / "image").mkdir()
    (bundle / "image" / "fixtures.json").write_text('{"fixtures": []}')
    shutil.copy(RUN_SH, bundle / "run.sh")
    fake = tmp_path / "fake"
    fake.mkdir()
    script(fake / "uname", 'case "$1" in -s) echo Darwin;; -m) echo arm64;; esac\n')
    env = {**os.environ, "PATH": f"{fake}:{os.environ['PATH']}"}
    args = ["sh", str(bundle / "run.sh")]
    for check_name in ("dispatch", "metal", "unit", "golden"):
        args += ["--skip", check_name]
    result = subprocess.run(args, env=env, capture_output=True, text=True, check=False)
    report = json.loads(result.stdout)
    assert result.returncode in (1, 2) and "verdict" in result.stderr
    assert report["schema_version"] == "2"
    assert report["metal_gate"]["status"] == "not_run"
    assert report["metal_rows"]["status"] == "not_applicable"
    assert "sandbox-exec" in result.stderr
