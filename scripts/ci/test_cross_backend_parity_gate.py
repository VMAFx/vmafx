#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for cross_backend_parity_gate.py (T6-8 / ADR-0214).

All tests are pure-Python — no vmaf binary, no GPU, no YUV fixtures.
They exercise the data-processing and command-building logic directly.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from cross_backend_calibration import CalibrationEntry, CalibrationTable
from cross_backend_parity_gate import (
    BACKEND_EXTRACTOR_ALIASES,
    BACKEND_SUFFIX,
    DEFAULT_FP16_TOLERANCE,
    DEFAULT_FP32_TOLERANCE,
    FEATURE_METRICS,
    FEATURE_TOLERANCE,
    Cell,
    CellResult,
    build_command,
    build_matrix,
    diff_frames,
    emit_json,
    emit_md,
    feature_extractor_name,
    resolve_cell_tolerance,
)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _ok_result(feature: str = "vif", backend_a: str = "cpu", backend_b: str = "cuda") -> CellResult:
    metrics = FEATURE_METRICS[feature]
    return CellResult(
        feature=feature,
        backend_a=backend_a,
        backend_b=backend_b,
        tolerance=FEATURE_TOLERANCE.get(feature, DEFAULT_FP32_TOLERANCE),
        n_frames=3,
        per_metric_max=dict.fromkeys(metrics, 0.0),
        per_metric_mismatches=dict.fromkeys(metrics, 0),
        status="OK",
    )


def _fail_result(feature: str = "vif") -> CellResult:
    metrics = FEATURE_METRICS[feature]
    return CellResult(
        feature=feature,
        backend_a="cpu",
        backend_b="cuda",
        tolerance=FEATURE_TOLERANCE.get(feature, DEFAULT_FP32_TOLERANCE),
        n_frames=3,
        per_metric_max=dict.fromkeys(metrics, 0.001),
        per_metric_mismatches=dict.fromkeys(metrics, 1),
        status="FAIL",
    )


def _error_result(feature: str = "vif", note: str = "binary failed") -> CellResult:
    metrics = FEATURE_METRICS[feature]
    return CellResult(
        feature=feature,
        backend_a="cpu",
        backend_b="cuda",
        tolerance=FEATURE_TOLERANCE.get(feature, DEFAULT_FP32_TOLERANCE),
        n_frames=0,
        per_metric_max=dict.fromkeys(metrics, 0.0),
        per_metric_mismatches=dict.fromkeys(metrics, 0),
        status="ERROR",
        note=note,
    )


def _calibration_table(*patterns_features: tuple[str, dict[str, float]]) -> CalibrationTable:
    entries = [
        CalibrationEntry(
            gpu_id_pattern=pat,
            label=pat,
            status="calibrated",
            features=feats,
        )
        for pat, feats in patterns_features
    ]
    return CalibrationTable(
        version=1,
        default_fp32_tolerance=DEFAULT_FP32_TOLERANCE,
        default_fp16_tolerance=DEFAULT_FP16_TOLERANCE,
        entries=entries,
    )


def _placeholder_table(pattern: str) -> CalibrationTable:
    entries = [
        CalibrationEntry(
            gpu_id_pattern=pattern,
            label=pattern,
            status="placeholder",
            features={},
        )
    ]
    return CalibrationTable(
        version=1,
        default_fp32_tolerance=DEFAULT_FP32_TOLERANCE,
        default_fp16_tolerance=DEFAULT_FP16_TOLERANCE,
        entries=entries,
    )


# ---------------------------------------------------------------------------
# build_matrix
# ---------------------------------------------------------------------------


def test_build_matrix_single_pair() -> None:
    cells = build_matrix(["vif"], ["cpu", "cuda"])
    assert len(cells) == 1
    assert cells[0] == Cell(feature="vif", backend_a="cpu", backend_b="cuda")


def test_build_matrix_two_features() -> None:
    features_in = ["vif", "psnr"]
    cells = build_matrix(features_in, ["cpu", "cuda"])
    assert len(cells) == len(features_in)
    features = {c.feature for c in cells}
    assert features == {"vif", "psnr"}


def test_build_matrix_three_backends_produces_three_pairs() -> None:
    backends = ["cpu", "cuda", "vulkan"]
    expected_pairs = {("cpu", "cuda"), ("cpu", "vulkan"), ("cuda", "vulkan")}
    cells = build_matrix(["vif"], backends)
    # C(3,2) = 3 pairs
    assert len(cells) == len(expected_pairs)
    pairs = {(c.backend_a, c.backend_b) for c in cells}
    assert pairs == expected_pairs


def test_build_matrix_empty_features() -> None:
    cells = build_matrix([], ["cpu", "cuda"])
    assert cells == []


def test_build_matrix_single_backend_no_pairs() -> None:
    cells = build_matrix(["vif"], ["cpu"])
    assert cells == []


def test_build_matrix_no_duplicate_pairs() -> None:
    cells = build_matrix(["vif"], ["cpu", "cuda", "vulkan", "sycl"])
    pairs = [(c.backend_a, c.backend_b) for c in cells]
    # Every pair should be (earlier, later) in the input list — no (cuda, cpu).
    for a, b in pairs:
        backend_list = ["cpu", "cuda", "vulkan", "sycl"]
        assert backend_list.index(a) < backend_list.index(b)


# ---------------------------------------------------------------------------
# feature_extractor_name
# ---------------------------------------------------------------------------


def test_feature_extractor_name_cpu_has_no_suffix() -> None:
    assert feature_extractor_name("vif", "cpu") == "vif"


def test_feature_extractor_name_cuda_has_cuda_suffix() -> None:
    assert feature_extractor_name("vif", "cuda") == "vif_cuda"


def test_feature_extractor_name_sycl_has_sycl_suffix() -> None:
    assert feature_extractor_name("psnr", "sycl") == "psnr_sycl"


def test_feature_extractor_name_vulkan_has_vulkan_suffix() -> None:
    assert feature_extractor_name("float_ssim", "vulkan") == "float_ssim_vulkan"


def test_feature_extractor_name_alias_adm_vulkan() -> None:
    # ADR-0586: Vulkan integer ADM uses canonical renamed extractor.
    assert feature_extractor_name("adm", "vulkan") == "integer_adm_vulkan"


def test_feature_extractor_name_alias_motion_vulkan() -> None:
    # ADR-0662: Vulkan motion uses integer_motion_vulkan.
    assert feature_extractor_name("motion", "vulkan") == "integer_motion_vulkan"


def test_feature_extractor_name_lcs_pseudo_feature_cpu() -> None:
    # float_ms_ssim_lcs → float_ms_ssim=enable_lcs=true on CPU.
    result = feature_extractor_name("float_ms_ssim_lcs", "cpu")
    assert result == "float_ms_ssim=enable_lcs=true"


def test_feature_extractor_name_lcs_pseudo_feature_cuda() -> None:
    # float_ms_ssim_lcs → float_ms_ssim_cuda=enable_lcs=true on CUDA.
    result = feature_extractor_name("float_ms_ssim_lcs", "cuda")
    assert result == "float_ms_ssim_cuda=enable_lcs=true"


def test_backend_extractor_aliases_are_consistent_with_backend_suffix() -> None:
    # Aliases should always resolve to something that does NOT simply use BACKEND_SUFFIX.
    for (feat, backend), alias in BACKEND_EXTRACTOR_ALIASES.items():
        plain = f"{feat}{BACKEND_SUFFIX[backend]}"
        assert alias != plain, (
            f"Alias ({feat}, {backend}) → {alias!r} is identical to the plain name {plain!r}; "
            "the alias entry is unnecessary"
        )


# ---------------------------------------------------------------------------
# build_command
# ---------------------------------------------------------------------------


def test_build_command_cpu_no_device_flag(tmp_path: Path) -> None:
    cmd = build_command(
        binary=tmp_path / "vmaf",
        ref=tmp_path / "ref.yuv",
        dist=tmp_path / "dist.yuv",
        width=1920,
        height=1080,
        pix_fmt="420",
        bitdepth=8,
        feature="vif",
        backend="cpu",
        device=None,
        output=tmp_path / "out.json",
    )
    assert "--backend" in cmd
    idx = cmd.index("--backend")
    assert cmd[idx + 1] == "cpu"
    # CPU should not inject a device flag.
    assert "--gpumask" not in cmd
    assert "--sycl_device" not in cmd
    assert "--vulkan_device" not in cmd


def test_build_command_cuda_includes_gpumask(tmp_path: Path) -> None:
    cmd = build_command(
        binary=tmp_path / "vmaf",
        ref=tmp_path / "ref.yuv",
        dist=tmp_path / "dist.yuv",
        width=640,
        height=480,
        pix_fmt="420",
        bitdepth=8,
        feature="vif",
        backend="cuda",
        device=1,
        output=tmp_path / "out.json",
    )
    assert "--gpumask" in cmd
    idx = cmd.index("--gpumask")
    assert cmd[idx + 1] == "1"


def test_build_command_vulkan_includes_vulkan_device(tmp_path: Path) -> None:
    cmd = build_command(
        binary=tmp_path / "vmaf",
        ref=tmp_path / "ref.yuv",
        dist=tmp_path / "dist.yuv",
        width=320,
        height=240,
        pix_fmt="420",
        bitdepth=8,
        feature="vif",
        backend="vulkan",
        device=0,
        output=tmp_path / "out.json",
    )
    assert "--vulkan_device" in cmd


def test_build_command_no_prediction_flag_present(tmp_path: Path) -> None:
    cmd = build_command(
        binary=tmp_path / "vmaf",
        ref=tmp_path / "ref.yuv",
        dist=tmp_path / "dist.yuv",
        width=64,
        height=64,
        pix_fmt="420",
        bitdepth=8,
        feature="psnr",
        backend="cpu",
        device=None,
        output=tmp_path / "out.json",
    )
    assert "--no_prediction" in cmd
    assert "--json" in cmd


def test_build_command_device_none_cpu_skips_device_flag(tmp_path: Path) -> None:
    # device=None + backend=cuda: should not inject device flag either
    # (run_one callers pass device from devices dict which may lack the key).
    cmd = build_command(
        binary=tmp_path / "vmaf",
        ref=tmp_path / "ref.yuv",
        dist=tmp_path / "dist.yuv",
        width=64,
        height=64,
        pix_fmt="420",
        bitdepth=8,
        feature="vif",
        backend="cuda",
        device=None,
        output=tmp_path / "out.json",
    )
    # device=None → no device flag injected
    assert "--gpumask" not in cmd


# ---------------------------------------------------------------------------
# diff_frames
# ---------------------------------------------------------------------------


def _make_frame(metrics: dict[str, float]) -> dict:
    return {"metrics": metrics}


def test_diff_frames_identical_frames_returns_zero_max() -> None:
    metrics = ("integer_vif_scale0", "integer_vif_scale1")
    frames_a = [_make_frame(dict.fromkeys(metrics, 0.5))]
    frames_b = [_make_frame(dict.fromkeys(metrics, 0.5))]
    per_max, per_mismatch = diff_frames(frames_a, frames_b, metrics, tolerance=5e-5)
    assert all(v == 0.0 for v in per_max.values())
    assert all(c == 0 for c in per_mismatch.values())


def test_diff_frames_detects_exceeding_tolerance() -> None:
    metrics = ("integer_vif_scale0",)
    frames_a = [_make_frame({"integer_vif_scale0": 0.5})]
    frames_b = [_make_frame({"integer_vif_scale0": 0.5 + 1e-3})]
    per_max, per_mismatch = diff_frames(frames_a, frames_b, metrics, tolerance=5e-5)
    assert per_max["integer_vif_scale0"] == pytest.approx(1e-3)
    assert per_mismatch["integer_vif_scale0"] == 1


def test_diff_frames_within_tolerance_no_mismatch() -> None:
    metrics = ("psnr_y",)
    frames_a = [_make_frame({"psnr_y": 40.0})]
    frames_b = [_make_frame({"psnr_y": 40.0 + 1e-6})]
    _per_max, per_mismatch = diff_frames(frames_a, frames_b, metrics, tolerance=5e-5)
    assert per_mismatch["psnr_y"] == 0


def test_diff_frames_accumulates_max_across_frames() -> None:
    metrics = ("integer_vif_scale0",)
    frames_a = [
        _make_frame({"integer_vif_scale0": 0.5}),
        _make_frame({"integer_vif_scale0": 0.6}),
        _make_frame({"integer_vif_scale0": 0.7}),
    ]
    frames_b = [
        _make_frame({"integer_vif_scale0": 0.5 + 1e-4}),
        _make_frame({"integer_vif_scale0": 0.6 + 5e-4}),  # largest diff
        _make_frame({"integer_vif_scale0": 0.7 + 2e-4}),
    ]
    per_max, per_mismatch = diff_frames(frames_a, frames_b, metrics, tolerance=5e-5)
    assert per_max["integer_vif_scale0"] == pytest.approx(5e-4)
    # All three frames exceed tolerance=5e-5.
    n_frames = len(frames_a)
    assert per_mismatch["integer_vif_scale0"] == n_frames


def test_diff_frames_mismatched_lengths_raises() -> None:
    metrics = ("psnr_y",)
    frames_a = [_make_frame({"psnr_y": 40.0}), _make_frame({"psnr_y": 38.0})]
    frames_b = [_make_frame({"psnr_y": 40.0})]
    with pytest.raises((ValueError, Exception)):
        diff_frames(frames_a, frames_b, metrics, tolerance=5e-5)


# ---------------------------------------------------------------------------
# resolve_cell_tolerance
# ---------------------------------------------------------------------------


def test_resolve_cell_tolerance_fp16_feature_overrides_all() -> None:
    tol, src = resolve_cell_tolerance(
        "vif",
        fp16_features=["vif"],
        calibration=None,
        gpu_id=None,
    )
    assert tol == pytest.approx(DEFAULT_FP16_TOLERANCE)
    assert src == "fp16"


def test_resolve_cell_tolerance_no_calibration_returns_feature_default() -> None:
    expected = FEATURE_TOLERANCE["vif"]
    tol, src = resolve_cell_tolerance(
        "vif",
        fp16_features=[],
        calibration=None,
        gpu_id=None,
    )
    assert tol == pytest.approx(expected)
    assert src == "default"


def test_resolve_cell_tolerance_no_gpu_id_returns_default() -> None:
    table = _calibration_table(("vulkan:0x10005:*", {"vif": 1e-6}))
    tol, src = resolve_cell_tolerance(
        "vif",
        fp16_features=[],
        calibration=table,
        gpu_id=None,
    )
    assert tol == pytest.approx(FEATURE_TOLERANCE["vif"])
    assert src == "default"


def test_resolve_cell_tolerance_calibrated_override() -> None:
    table = _calibration_table(("vulkan:0x10005:*", {"vif": 1.5e-5}))
    tol, src = resolve_cell_tolerance(
        "vif",
        fp16_features=[],
        calibration=table,
        gpu_id="vulkan:0x10005:0x0",
    )
    assert tol == pytest.approx(1.5e-5)
    assert "calibrated" in src
    assert "vulkan:0x10005:*" in src


def test_resolve_cell_tolerance_no_match_returns_no_calibration_label() -> None:
    table = _calibration_table(("cuda:8.6", {"vif": 1e-6}))
    tol, src = resolve_cell_tolerance(
        "vif",
        fp16_features=[],
        calibration=table,
        gpu_id="vulkan:0x10005:0x0",
    )
    assert tol == pytest.approx(FEATURE_TOLERANCE["vif"])
    assert "no-calibration" in src


def test_resolve_cell_tolerance_placeholder_entry_falls_back_to_feature_default() -> None:
    table = _placeholder_table("cuda:8.6")
    tol, src = resolve_cell_tolerance(
        "vif",
        fp16_features=[],
        calibration=table,
        gpu_id="cuda:8.6",
    )
    # Placeholder row with no per-feature override → feature default.
    assert tol == pytest.approx(FEATURE_TOLERANCE["vif"])
    assert "placeholder" in src


def test_resolve_cell_tolerance_unknown_feature_uses_fp32_default() -> None:
    tol, src = resolve_cell_tolerance(
        "nonexistent_feature",
        fp16_features=[],
        calibration=None,
        gpu_id=None,
    )
    assert tol == pytest.approx(DEFAULT_FP32_TOLERANCE)
    assert src == "default"


def test_resolve_cell_tolerance_calibrated_with_feature_override_uses_override() -> None:
    table = _calibration_table(("cuda:8.6", {"ciede": 3.0e-3}))
    tol, src = resolve_cell_tolerance(
        "ciede",
        fp16_features=[],
        calibration=table,
        gpu_id="cuda:8.6",
    )
    assert tol == pytest.approx(3.0e-3)
    assert "calibrated" in src


# ---------------------------------------------------------------------------
# emit_json
# ---------------------------------------------------------------------------


def test_emit_json_schema_version(tmp_path: Path) -> None:
    results = [_ok_result()]
    out = tmp_path / "out.json"
    emit_json(results, out)
    payload = json.loads(out.read_text(encoding="utf-8"))
    assert payload["schema_version"] == 1


def test_emit_json_one_record_per_result(tmp_path: Path) -> None:
    results = [_ok_result("vif"), _ok_result("psnr")]
    out = tmp_path / "out.json"
    emit_json(results, out)
    payload = json.loads(out.read_text(encoding="utf-8"))
    assert len(payload["cells"]) == len(results)


def test_emit_json_fields_present(tmp_path: Path) -> None:
    results = [_ok_result()]
    out = tmp_path / "out.json"
    emit_json(results, out)
    cell = json.loads(out.read_text(encoding="utf-8"))["cells"][0]
    required = {
        "feature",
        "backend_a",
        "backend_b",
        "tolerance_abs",
        "tolerance_source",
        "n_frames",
        "status",
        "note",
        "per_metric_max_abs_diff",
        "per_metric_mismatches",
    }
    assert required <= set(cell.keys())


def test_emit_json_fail_status_recorded(tmp_path: Path) -> None:
    results = [_fail_result()]
    out = tmp_path / "out.json"
    emit_json(results, out)
    cell = json.loads(out.read_text(encoding="utf-8"))["cells"][0]
    assert cell["status"] == "FAIL"


def test_emit_json_file_ends_with_newline(tmp_path: Path) -> None:
    out = tmp_path / "out.json"
    emit_json([_ok_result()], out)
    raw = out.read_bytes()
    assert raw.endswith(b"\n")


# ---------------------------------------------------------------------------
# emit_md
# ---------------------------------------------------------------------------


def test_emit_md_contains_header(tmp_path: Path) -> None:
    out = tmp_path / "out.md"
    emit_md([_ok_result()], out)
    text = out.read_text(encoding="utf-8")
    assert "Cross-backend parity gate" in text


def test_emit_md_table_row_per_result(tmp_path: Path) -> None:
    out = tmp_path / "out.md"
    results = [_ok_result("vif"), _ok_result("psnr")]
    emit_md(results, out)
    text = out.read_text(encoding="utf-8")
    assert "`vif`" in text
    assert "`psnr`" in text


def test_emit_md_failures_detail_section_present(tmp_path: Path) -> None:
    out = tmp_path / "out.md"
    emit_md([_fail_result()], out)
    text = out.read_text(encoding="utf-8")
    assert "Failures detail" in text


def test_emit_md_error_result_in_failure_section(tmp_path: Path) -> None:
    out = tmp_path / "out.md"
    emit_md([_error_result(note="backend_a cpu failed: exit 127")], out)
    text = out.read_text(encoding="utf-8")
    assert "ERROR" in text


def test_emit_md_no_failure_section_when_all_ok(tmp_path: Path) -> None:
    out = tmp_path / "out.md"
    emit_md([_ok_result("vif"), _ok_result("psnr")], out)
    text = out.read_text(encoding="utf-8")
    assert "Failures detail" not in text


def test_emit_md_tolerance_source_appears(tmp_path: Path) -> None:
    result = _ok_result()
    result = CellResult(
        feature=result.feature,
        backend_a=result.backend_a,
        backend_b=result.backend_b,
        tolerance=result.tolerance,
        n_frames=result.n_frames,
        per_metric_max=result.per_metric_max,
        per_metric_mismatches=result.per_metric_mismatches,
        status=result.status,
        tolerance_source="calibrated:vulkan:0x10005:*",
    )
    out = tmp_path / "out.md"
    emit_md([result], out)
    text = out.read_text(encoding="utf-8")
    assert "calibrated:vulkan:0x10005:*" in text


# ---------------------------------------------------------------------------
# FEATURE_METRICS completeness check
# ---------------------------------------------------------------------------


def test_every_feature_in_feature_tolerance_is_in_feature_metrics() -> None:
    """All keys in FEATURE_TOLERANCE must appear in FEATURE_METRICS."""
    missing = set(FEATURE_TOLERANCE) - set(FEATURE_METRICS)
    assert (
        missing == set()
    ), f"Features in FEATURE_TOLERANCE but missing from FEATURE_METRICS: {missing}"


def test_feature_metrics_values_are_non_empty_tuples() -> None:
    for feature, metrics in FEATURE_METRICS.items():
        assert isinstance(metrics, tuple), f"{feature}: FEATURE_METRICS value must be a tuple"
        assert len(metrics) >= 1, f"{feature}: FEATURE_METRICS must have at least one metric name"
