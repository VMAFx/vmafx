#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

"""Regression tests for testdata/run_sycl_scores.py (BUG-040)."""

from __future__ import annotations

import json
from pathlib import Path
from unittest.mock import MagicMock

import pytest

from testdata import run_sycl_scores


def test_build_vmaf_cmd_uses_explicit_sycl_backend() -> None:
    cmd = run_sycl_scores.build_vmaf_cmd(
        vmaf_bin="/usr/local/bin/vmaf",
        ref="ref.yuv",
        dis="dis.yuv",
        width=1920,
        height=1080,
        model="version=vmaf_v0.6.1",
        output_path="scores.json",
        backend="sycl",
    )

    assert "--backend" in cmd
    idx = cmd.index("--backend")
    assert cmd[idx + 1] == "sycl"
    assert "--no_cuda" not in cmd


def test_build_vmaf_cmd_optional_device() -> None:
    cmd = run_sycl_scores.build_vmaf_cmd(
        vmaf_bin="/usr/local/bin/vmaf",
        ref="ref.yuv",
        dis="dis.yuv",
        width=1280,
        height=720,
        model="version=vmaf_v0.6.1",
        output_path="scores.json",
        backend="sycl",
        device=0,
    )

    assert "--sycl_device" in cmd
    dev_idx = cmd.index("--sycl_device")
    assert cmd[dev_idx + 1] == "0"


def test_bug040_regression_no_cuda_flag_prohibited() -> None:
    """BUG-040: negative --no_cuda flag failed to select SYCL; verify exclusive --backend sycl."""
    cmd = run_sycl_scores.build_vmaf_cmd(
        vmaf_bin="vmaf",
        ref="r.yuv",
        dis="d.yuv",
        width=576,
        height=324,
        model="version=vmaf_v0.6.1",
        output_path="out.json",
    )
    assert "--no_cuda" not in cmd
    assert "--backend" in cmd
    assert cmd[cmd.index("--backend") + 1] == "sycl"


def test_run_single_resolution_skips_missing_files(tmp_path: Path) -> None:
    ok = run_sycl_scores.run_single_resolution(
        vmaf_bin="vmaf",
        dims="1280x720",
        tag="720",
        gpu_tag="a380",
        basedir=tmp_path,
    )
    assert ok is False


def test_compare_vs_cpu_computes_max_delta(tmp_path: Path) -> None:
    cpu_path = tmp_path / "scores_cpu_test.json"
    cpu_content = {
        "frames": [
            {"metrics": {"vmaf": 90.0}},
            {"metrics": {"vmaf": 95.0}},
        ]
    }
    cpu_path.write_text(json.dumps(cpu_content), encoding="utf-8")

    sycl_content = {
        "frames": [
            {"metrics": {"vmaf": 90.0002}},
            {"metrics": {"vmaf": 94.9997}},
        ]
    }

    max_diff = run_sycl_scores.compare_vs_cpu(tmp_path, "test", sycl_content)
    assert max_diff is not None
    assert pytest.approx(max_diff, rel=1e-5) == 0.0003


def test_prepare_sycl_env_pins_device_and_ld_path(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv("ONEAPI_DEVICE_SELECTOR", raising=False)
    monkeypatch.setenv("LD_LIBRARY_PATH", "/opt/intel/lib")

    env = run_sycl_scores.prepare_sycl_env()
    assert env["ONEAPI_DEVICE_SELECTOR"] == "level_zero:gpu"
    assert "/usr/local/lib" in env["LD_LIBRARY_PATH"]
    assert "/opt/intel/lib" in env["LD_LIBRARY_PATH"]


def test_prepare_sycl_env_preserves_existing_overrides(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("ONEAPI_DEVICE_SELECTOR", "level_zero:0")

    env = run_sycl_scores.prepare_sycl_env()
    assert env["ONEAPI_DEVICE_SELECTOR"] == "level_zero:0"
