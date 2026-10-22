#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Contract tests for scripts/test/zerocopy_e2e_compare.py.

Pure Python: synthetic per-frame JSON plus ``.rc`` / ``.err`` files stand in for the
three legs (CPU libvmaf, libvmaf_sycl host upload, libvmaf_sycl zero-copy).
"""

from __future__ import annotations

import dataclasses
import json
import sys
from pathlib import Path
from typing import Any

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.test import zerocopy_e2e_compare as zc

MIN_CASES = 24
STAGE_1, STAGE_2, STAGE_3 = 1, 2, 3
CLIP = "src01_8bit"
STAGE1_CASE = "vif"
STAGE3_CASE = "float_ssim"
METRICS = {"integer_vif_scale0": 0.5, "integer_vif_scale1": 0.25}


def _frames(metrics: dict[str, Any], n: int = 3) -> dict[str, Any]:
    return {"frames": [{"frameNum": i, "metrics": dict(metrics)} for i in range(n)]}


def _leg(d: Path, case: str, leg: str, metrics: dict[str, Any] | None, rc: int, err: str) -> None:
    base = d / f"{CLIP}__{case}.{leg}"
    if metrics is not None:
        base.with_name(base.name + ".json").write_text(json.dumps(_frames(metrics)))
    base.with_name(base.name + ".rc").write_text(f"{rc}\n")
    base.with_name(base.name + ".err").write_text(err)


def _case(
    d: Path,
    case: str,
    cpu: dict[str, Any],
    host: dict[str, Any],
    zc_metrics: dict[str, Any] | None,
    zc_rc: int = 0,
    zc_err: str = "",
) -> None:
    _leg(d, case, "cpu", cpu, 0, "")
    _leg(d, case, "host", host, 0, "")
    _leg(d, case, "zc", zc_metrics, zc_rc, zc_err)


def _run(d: Path, stage: int, case: str, capsys: pytest.CaptureFixture[str]) -> tuple[int, str]:
    rc = zc.main(["--stage", str(stage), "--dir", str(d), "--cases", case])
    return rc, capsys.readouterr().out


def test_stage1_feature_all_equal_passes(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    _case(tmp_path, STAGE1_CASE, METRICS, METRICS, METRICS)
    rc, out = _run(tmp_path, 1, STAGE1_CASE, capsys)
    assert rc == 0
    assert f"ZC-E2E src01 8 {STAGE1_CASE} PASS" in out
    assert "ZC-E2E SUMMARY stage=1 pass=1 fail=0 nonexact=0" in out


def test_missing_metric_in_zero_copy_is_silent_drop(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    dropped = {"integer_vif_scale0": 0.5}
    _case(tmp_path, STAGE1_CASE, METRICS, METRICS, dropped)
    rc, out = _run(tmp_path, 1, STAGE1_CASE, capsys)
    assert rc == 1
    assert "FAIL silent-drop" in out
    assert "integer_vif_scale1" in out
    assert "ZC-E2E SUMMARY stage=1 pass=0 fail=1 nonexact=0" in out


def test_zero_copy_differs_from_host(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    off = {"integer_vif_scale0": 0.5 + 1e-6, "integer_vif_scale1": 0.25}
    _case(tmp_path, STAGE1_CASE, METRICS, METRICS, off)
    rc, out = _run(tmp_path, 1, STAGE1_CASE, capsys)
    assert rc == 1
    assert "FAIL zc-vs-host" in out
    assert "integer_vif_scale0" in out


def test_host_differs_from_cpu_is_nonexact(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    off = {"integer_vif_scale0": 0.5 + 1e-6, "integer_vif_scale1": 0.25}
    _case(tmp_path, STAGE1_CASE, METRICS, off, off)
    rc, out = _run(tmp_path, 1, STAGE1_CASE, capsys)
    assert rc == 1
    assert "NONEXACT host-vs-cpu" in out
    assert "tolerance(info)=" in out
    assert "ZC-E2E SUMMARY stage=1 pass=0 fail=0 nonexact=1" in out


def test_later_stage_feature_failing_loudly_passes(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    m = {"float_ssim": 0.9}
    _case(
        tmp_path,
        STAGE3_CASE,
        m,
        m,
        None,
        zc_rc=1,
        zc_err="float_ssim_sycl: needs host pictures (-ENOTSUP)\n",
    )
    rc, out = _run(tmp_path, 1, STAGE3_CASE, capsys)
    assert rc == 0
    assert "PASS loud-fail" in out


def test_later_stage_feature_succeeding_is_unexpected(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    m = {"float_ssim": 0.9}
    _case(tmp_path, STAGE3_CASE, m, m, m, zc_rc=0)
    rc, out = _run(tmp_path, 1, STAGE3_CASE, capsys)
    assert rc == 1
    assert "FAIL unexpected-success" in out


def test_later_stage_failure_must_name_the_feature(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    m = {"float_ssim": 0.9}
    _case(tmp_path, STAGE3_CASE, m, m, None, zc_rc=1, zc_err="Conversion failed!\n")
    rc, out = _run(tmp_path, 1, STAGE3_CASE, capsys)
    assert rc == 1
    assert "FAIL unnamed-failure" in out


def test_later_stage_feature_is_numeric_parity_at_its_stage(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    m = {"float_ssim": 0.9}
    _case(tmp_path, STAGE3_CASE, m, m, {}, zc_rc=0)
    rc, out = _run(tmp_path, 3, STAGE3_CASE, capsys)
    assert rc == 1
    assert "FAIL silent-drop" in out


def test_failed_zero_copy_leg_at_its_stage_fails(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    _case(tmp_path, STAGE1_CASE, METRICS, METRICS, None, zc_rc=1, zc_err="boom\n")
    rc, out = _run(tmp_path, 1, STAGE1_CASE, capsys)
    assert rc == 1
    assert "FAIL zc-failed" in out


def test_missing_leg_files_fail(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    _leg(tmp_path, STAGE1_CASE, "cpu", METRICS, 0, "")
    rc, out = _run(tmp_path, 1, STAGE1_CASE, capsys)
    assert rc == 1
    assert "FAIL missing-leg" in out


def test_empty_directory_is_not_a_pass(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    rc, out = _run(tmp_path, 1, STAGE1_CASE, capsys)
    assert rc == 1
    assert "pass=0" in out


def test_parity_stage_table_shape() -> None:
    assert len(zc.PARITY_STAGE) >= MIN_CASES
    assert set(zc.PARITY_STAGE.values()) == {STAGE_1, STAGE_2, STAGE_3}
    assert zc.PARITY_STAGE["model-vmaf_v0.6.1"] == STAGE_1
    assert zc.PARITY_STAGE["model-vmaf_float_v0.6.1"] == STAGE_3
    assert zc.PARITY_STAGE["psnr"] == STAGE_2
    assert zc.PARITY_STAGE["psnr_luma"] == STAGE_1
    assert set(zc.CASES) == set(zc.PARITY_STAGE)


def test_dotted_case_id_keeps_every_leg_file_distinct(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    case = "model-vmaf_v0.6.1"
    _case(tmp_path, case, {"vmaf": 90.0}, {"vmaf": 90.0}, {"vmaf": 90.5})
    rc, out = _run(tmp_path, 1, case, capsys)
    assert rc == 1
    assert "FAIL zc-vs-host" in out


def test_known_sycl_twin_omission_is_not_a_drop_but_other_metrics_are(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    omitted = "VMAF_integer_feature_motion_sad_score"
    cpu = {**METRICS, omitted: 1.0}
    _case(tmp_path, STAGE1_CASE, cpu, METRICS, METRICS)
    rc, out = _run(tmp_path, 1, STAGE1_CASE, capsys)
    assert rc == 0, out
    assert omitted in zc.SYCL_TWIN_OMITTED
    assert {omitted} == zc.SYCL_TWIN_OMITTED


def test_repeated_zero_copy_runs_that_agree_pass(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    _case(tmp_path, STAGE1_CASE, METRICS, METRICS, METRICS)
    for k in (2, 3):
        _leg(tmp_path, STAGE1_CASE, f"zc-r{k}", METRICS, 0, "")
    rc, out = _run(tmp_path, 1, STAGE1_CASE, capsys)
    assert rc == 0, out
    assert f"ZC-E2E src01 8 {STAGE1_CASE} PASS" in out


def test_one_divergent_repeat_is_nondeterministic(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    off = {"integer_vif_scale0": 0.5, "integer_vif_scale1": 0.25 + 1e-6}
    _case(tmp_path, STAGE1_CASE, METRICS, METRICS, METRICS)
    _leg(tmp_path, STAGE1_CASE, "zc-r2", METRICS, 0, "")
    _leg(tmp_path, STAGE1_CASE, "zc-r3", off, 0, "")
    rc, out = _run(tmp_path, 1, STAGE1_CASE, capsys)
    assert rc == 1
    assert "FAIL zc-nondeterministic 1 of 3 runs differ" in out
    assert "zc-r3: zc-vs-host integer_vif_scale1" in out
    assert "zc-r2:" not in out


def test_failed_repeat_is_nondeterministic(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    _case(tmp_path, STAGE1_CASE, METRICS, METRICS, METRICS)
    _leg(tmp_path, STAGE1_CASE, "zc-r2", None, 1, "vmaf_read_pictures_sycl failed: -5\n")
    rc, out = _run(tmp_path, 1, STAGE1_CASE, capsys)
    assert rc == 1
    assert "FAIL zc-nondeterministic" in out
    assert "zc-r2: zc-failed rc=1" in out


def test_repeats_order_numerically_and_dotted_ids_work(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    case = "model-vmaf_v0.6.1"
    runs, divergent = 11, 10
    _case(tmp_path, case, {"vmaf": 90.0}, {"vmaf": 90.0}, {"vmaf": 90.0})
    for k in range(2, runs + 1):
        _leg(tmp_path, case, f"zc-r{k}", {"vmaf": 91.0 if k == divergent else 90.0}, 0, "")
    bases = zc._repeat_bases(tmp_path, f"{CLIP}__{case}")
    assert [b.name.rsplit(".", 1)[-1] for b in bases] == [f"zc-r{k}" for k in range(2, runs + 1)]
    rc, out = _run(tmp_path, 1, case, capsys)
    assert rc == 1
    assert "1 of 11 runs differ; zc-r10:" in out


def test_full_double_difference_is_nonexact(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """A last-bits difference only visible at ``score_fmt=%.17g`` is not exact."""

    host = {"integer_vif_scale0": 0.5 + 1e-15, "integer_vif_scale1": 0.25}
    _case(tmp_path, STAGE1_CASE, METRICS, host, host)
    rc, out = _run(tmp_path, 1, STAGE1_CASE, capsys)
    assert rc == 1
    assert "NONEXACT host-vs-cpu" in out


def test_declared_cpu_bound_is_applied_and_named(
    tmp_path: Path, capsys: pytest.CaptureFixture[str], monkeypatch: pytest.MonkeyPatch
) -> None:
    bounded = dataclasses.replace(zc.CASES[STAGE1_CASE], cpu_bound=1e-9)
    monkeypatch.setitem(zc.CASES, STAGE1_CASE, bounded)
    near = {"integer_vif_scale0": 0.5 + 1e-12, "integer_vif_scale1": 0.25}
    _case(tmp_path, STAGE1_CASE, METRICS, near, near)
    rc, out = _run(tmp_path, 1, STAGE1_CASE, capsys)
    assert rc == 0
    far = {"integer_vif_scale0": 0.5 + 1e-6, "integer_vif_scale1": 0.25}
    _case(tmp_path, STAGE1_CASE, METRICS, far, far)
    rc, out = _run(tmp_path, 1, STAGE1_CASE, capsys)
    assert rc == 1
    assert "exceeds declared bound 1e-09" in out
    assert "applied 1e-09" in out


HOST_REF_CASE = "motion_uv"
MOTION_UV = {"VMAF_integer_feature_motion2_score": 1.5, "VMAF_integer_feature_motion_uv_score": 0.5}


def _host_ref_case(d: Path, zc_metrics: dict[str, Any] | None, zc_rc: int = 0) -> None:
    """Host-reference case: no CPU leg is written at all."""

    _leg(d, HOST_REF_CASE, "host", MOTION_UV, 0, "")
    _leg(d, HOST_REF_CASE, "zc", zc_metrics, zc_rc, "")


def test_host_reference_case_needs_no_cpu_leg(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    assert zc.CASES[HOST_REF_CASE].reference == "host"
    _host_ref_case(tmp_path, MOTION_UV)
    rc, out = _run(tmp_path, 2, HOST_REF_CASE, capsys)
    assert rc == 0, out
    assert f"{HOST_REF_CASE} PASS" in out
    assert "host-vs-cpu" not in out


def test_host_reference_case_fails_when_zero_copy_differs(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    _host_ref_case(tmp_path, {**MOTION_UV, "VMAF_integer_feature_motion_uv_score": 0.5 + 1e-15})
    rc, out = _run(tmp_path, 2, HOST_REF_CASE, capsys)
    assert rc == 1
    assert "FAIL zc-vs-host" in out


def test_host_reference_case_repeats_are_held_to_host(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    _host_ref_case(tmp_path, MOTION_UV)
    drift = {**MOTION_UV, "VMAF_integer_feature_motion2_score": 9.0}
    _leg(tmp_path, HOST_REF_CASE, "zc-r2", drift, 0, "")
    rc, out = _run(tmp_path, 2, HOST_REF_CASE, capsys)
    assert rc == 1
    assert "zc-nondeterministic" in out


def test_ciede_declares_the_gate_libm_bound_and_everything_else_is_exact() -> None:
    bounded = {k: c.cpu_bound for k, c in zc.CASES.items() if c.cpu_bound}
    assert bounded == {"ciede": 1e-9}


def test_bounded_pass_names_measurement_and_bound(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    cpu = {"ciede2000": 33.10755745833333}
    host = {"ciede2000": 33.10755745833333 + 1.1e-11}
    _case(tmp_path, "ciede", cpu, host, host)
    rc, out = _run(tmp_path, 3, "ciede", capsys)
    assert rc == 0, out
    assert "PASS" in out
    assert "within declared bound 1e-09" in out
    assert "zc-vs-host" not in out


def test_bounded_case_still_requires_zero_copy_equal_to_host(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    cpu = {"ciede2000": 33.1}
    host = {"ciede2000": 33.1 + 1.1e-11}
    _case(tmp_path, "ciede", cpu, host, {"ciede2000": 33.1 + 2.2e-11})
    rc, out = _run(tmp_path, 3, "ciede", capsys)
    assert rc == 1
    assert "FAIL zc-vs-host" in out
