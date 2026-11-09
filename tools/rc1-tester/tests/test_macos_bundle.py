# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for the macOS bundle run script, on Linux with a fake `uname`."""

from __future__ import annotations

import json
import os
import shutil
import stat
import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaf_rc1_tester.safe_process import run_bounded

SCRIPT_TIMEOUT_S = 60
MAX_OUTPUT_BYTES = 4 * 1024 * 1024
NON_APPLE_SILICON_EXIT_CODE = 64

_ROOT = Path(__file__).resolve().parents[3]
RUN_SH = _ROOT / "tools" / "rc1-tester" / "image" / "macos" / "run.sh"

BASH = shutil.which("bash")
SH = shutil.which("sh")

pytestmark = pytest.mark.skipif(BASH is None or SH is None, reason="needs bash and sh")


def script(path: Path, body: str) -> None:
    path.write_text("#!/bin/sh\n" + body)
    path.chmod(path.stat().st_mode | stat.S_IEXEC)


def test_run_sh_refuses_other_platforms() -> None:
    """Assert run.sh exits with code 64 when invoked on non-Apple-silicon platforms."""
    result = run_bounded(
        [SH, str(RUN_SH)],
        timeout_seconds=SCRIPT_TIMEOUT_S,
        max_output_bytes=MAX_OUTPUT_BYTES,
    )
    assert result.returncode == NON_APPLE_SILICON_EXIT_CODE and "Apple silicon" in result.stderr


def test_run_sh_end_to_end_with_everything_skipped(tmp_path: Path) -> None:
    """Assert run.sh emits a valid JSON report when all checks are skipped."""
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
    args = [SH, str(bundle / "run.sh")]
    for check_name in ("dispatch", "metal", "unit", "golden"):
        args += ["--skip", check_name]
    result = run_bounded(
        args,
        environment=env,
        timeout_seconds=SCRIPT_TIMEOUT_S,
        max_output_bytes=MAX_OUTPUT_BYTES,
    )
    report = json.loads(result.stdout)
    assert result.returncode in (1, 2) and "verdict" in result.stderr
    assert report["schema_version"] == "4"
    assert report["metal_gate"]["status"] == "not_run"
    assert report["metal_rows"]["status"] == "not_applicable"
    assert "sandbox-exec" in result.stderr
