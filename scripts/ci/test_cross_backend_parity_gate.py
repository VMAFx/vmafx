#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Unit tests for cross_backend_parity_gate.py (T6-8 / ADR-0214).

All tests are pure-Python — no vmaf binary, no GPU, no YUV fixtures.
They exercise the data-processing and command-building logic directly.
"""

from __future__ import annotations

import json
import math
import re
import sys
from collections.abc import Iterator
from contextlib import contextmanager
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

import scripts.ci.cross_backend_parity_gate as parity_gate
from scripts.ci.cross_backend_calibration import (
    ADR_DIR,
    EXACT_TWIN_FRAGMENTS,
    EXACT_TWIN_SOURCE,
    EXACT_TWINS,
    EXACT_TWINS_DIR,
    LIBM_TWIN_SOURCE,
    LIBM_TWINS,
    CalibrationEntry,
    CalibrationTable,
    ExactTwinError,
    area_tolerance_factor,
    build_exact_twins,
    is_exact_pair,
    libm_pair_tolerance,
    load_exact_twin_fragments,
    psnr_hvs_term_count,
    validate_exact_twins,
)
from scripts.ci.cross_backend_parity_gate import (
    BACKEND_EXTRACTOR_ALIASES,
    BACKEND_SUFFIX,
    DEFAULT_FP16_TOLERANCE,
    DEFAULT_FP32_TOLERANCE,
    FEATURE_METRICS,
    FEATURE_TOLERANCE,
    HELD_EXACT_SOURCE,
    Cell,
    CellResult,
    build_command,
    build_matrix,
    chroma_plane_size,
    chroma_skip_note,
    diff_frames,
    emit_json,
    emit_md,
    feature_extractor_name,
    missing_metrics,
    resolve_cell_tolerance,
    run_cell,
)
from scripts.lib.safe_subprocess import run as run_command


def _close(actual: float, expected: float) -> bool:
    return math.isclose(actual, expected, rel_tol=1e-9, abs_tol=1e-15)


@contextmanager
def _raises(expected: type[BaseException]) -> Iterator[None]:
    try:
        yield
    except expected:
        return
    raise AssertionError(f"expected {expected.__name__}")


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
    backends = ["cpu", "cuda", "sycl"]
    expected_pairs = {("cpu", "cuda"), ("cpu", "sycl"), ("cuda", "sycl")}
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
    backend_list = ["cpu", "cuda", "sycl"]
    cells = build_matrix(["vif"], backend_list)
    pairs = [(c.backend_a, c.backend_b) for c in cells]
    # Every pair should be (earlier, later) in the input list — no (cuda, cpu).
    for a, b in pairs:
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


def test_feature_extractor_name_rejects_unknown_backend() -> None:
    with _raises(KeyError):
        feature_extractor_name("float_ssim", "unsupported")


def test_feature_extractor_name_lcs_pseudo_feature_cpu() -> None:
    # float_ms_ssim_lcs → float_ms_ssim=enable_lcs=true on CPU.
    result = feature_extractor_name("float_ms_ssim_lcs", "cpu")
    assert result == "float_ms_ssim=enable_lcs=true"


def test_feature_extractor_name_lcs_pseudo_feature_cuda() -> None:
    # float_ms_ssim_lcs → float_ms_ssim_cuda=enable_lcs=true on CUDA.
    result = feature_extractor_name("float_ms_ssim_lcs", "cuda")
    assert result == "float_ms_ssim_cuda=enable_lcs=true"


def test_feature_extractor_name_hip_has_hip_suffix() -> None:
    assert feature_extractor_name("float_ssim", "hip") == "float_ssim_hip"


def test_feature_extractor_name_float_ssim_lcs_cell() -> None:
    # ADR-1382: the enable_lcs cell runs the same extractor with the option.
    assert feature_extractor_name("float_ssim_lcs", "cpu") == "float_ssim=enable_lcs=true"
    assert feature_extractor_name("float_ssim_lcs", "hip") == "float_ssim_hip=enable_lcs=true"
    assert FEATURE_METRICS["float_ssim_lcs"] == (
        "float_ssim",
        "float_ssim_l",
        "float_ssim_c",
        "float_ssim_s",
    )
    assert FEATURE_TOLERANCE["float_ssim_lcs"] == DEFAULT_FP32_TOLERANCE


def test_feature_extractor_name_hip_ms_ssim_uses_its_registered_name() -> None:
    assert feature_extractor_name("float_ms_ssim", "hip") == "integer_ms_ssim_hip"
    assert (
        feature_extractor_name("float_ms_ssim_lcs", "hip") == "integer_ms_ssim_hip=enable_lcs=true"
    )


def test_float_ms_ssim_chroma_cell_runs_the_option_on_every_backend() -> None:
    # T-MS-SSIM-GPU-CHROMA-OPTION-DRIFT-2026-09-06: every twin scores chroma.
    assert (
        feature_extractor_name("float_ms_ssim_chroma", "cpu") == "float_ms_ssim=enable_chroma=true"
    )
    assert (
        feature_extractor_name("float_ms_ssim_chroma", "cuda")
        == "float_ms_ssim_cuda=enable_chroma=true"
    )
    assert (
        feature_extractor_name("float_ms_ssim_chroma", "hip")
        == "integer_ms_ssim_hip=enable_chroma=true"
    )
    assert FEATURE_METRICS["float_ms_ssim_chroma"] == (
        "float_ms_ssim",
        "float_ms_ssim_cb",
        "float_ms_ssim_cr",
    )
    assert FEATURE_TOLERANCE["float_ms_ssim_chroma"] == DEFAULT_FP32_TOLERANCE


def test_chroma_plane_size_is_ceil_subsampled() -> None:
    assert chroma_plane_size(576, 324, "420") == (288, 162)
    assert chroma_plane_size(351, 351, "420") == (176, 176)
    assert chroma_plane_size(351, 176, "422") == (176, 176)
    assert chroma_plane_size(176, 176, "444") == (176, 176)


def test_chroma_cell_skips_a_fixture_whose_chroma_is_too_small() -> None:
    # The CPU extractor refuses enable_chroma below 176 pixels of chroma.
    assert "288x162" in chroma_skip_note("float_ms_ssim_chroma", 576, 324, "420")
    assert "175x175" in chroma_skip_note("float_ms_ssim_chroma", 350, 350, "420")
    assert chroma_skip_note("float_ms_ssim_chroma", 351, 351, "420") == ""
    assert chroma_skip_note("float_ms_ssim_chroma", 576, 324, "422") == ""
    assert chroma_skip_note("float_ms_ssim_chroma", 576, 324, "444") == ""
    assert chroma_skip_note("float_ms_ssim", 576, 324, "420") == ""


def test_run_cell_skips_the_chroma_cell_without_running(tmp_path: Path, monkeypatch: Any) -> None:
    def fail_run_one(*_args: Any, **_kwargs: Any) -> tuple[int, str]:
        raise AssertionError("a skipped cell must not run vmaf")

    monkeypatch.setattr("scripts.ci.cross_backend_parity_gate.run_one", fail_run_one)
    result = run_cell(
        Cell(feature="float_ms_ssim_chroma", backend_a="cpu", backend_b="cuda"),
        binary=tmp_path / "vmaf",
        ref=tmp_path / "ref.yuv",
        dist=tmp_path / "dist.yuv",
        width=576,
        height=324,
        pix_fmt="420",
        bitdepth=8,
        workdir=tmp_path,
        devices={},
        tolerance=0.0,
    )
    assert result.status == "SKIP"
    assert "288x162" in result.note


def test_every_hip_cell_names_a_registered_hip_extractor() -> None:
    """The gate asks `--feature` for names the HIP build registers."""
    hip_dir = Path(__file__).resolve().parents[2] / "core" / "src" / "feature" / "hip"
    registered = set()
    for source in hip_dir.glob("*.c"):
        registered.update(
            re.findall(r'^\s*\.name = "([a-z0-9_]+_hip)"', source.read_text(encoding="utf-8"), re.M)
        )
    for feature in FEATURE_METRICS:
        extractor = feature_extractor_name(feature, "hip").split("=", 1)[0]
        assert extractor in registered, f"{feature}: no HIP extractor named {extractor!r}"


def test_ssim_twins_are_named_after_the_cpu_file() -> None:
    """ADR-1424: `ssim` is integer_ssim.c; no backend registers `ssim_<backend>`."""
    assert feature_extractor_name("ssim", "cpu") == "ssim"
    assert feature_extractor_name("ssim", "cuda") == "integer_ssim_cuda"
    assert feature_extractor_name("ssim", "sycl") == "integer_ssim_sycl"
    assert feature_extractor_name("ssim", "hip") == "integer_ssim_hip"
    assert FEATURE_METRICS["ssim"] == ("ssim",)


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


def test_build_command_sycl_includes_sycl_device(tmp_path: Path) -> None:
    cmd = build_command(
        binary=tmp_path / "vmaf",
        ref=tmp_path / "ref.yuv",
        dist=tmp_path / "dist.yuv",
        width=320,
        height=240,
        pix_fmt="420",
        bitdepth=8,
        feature="vif",
        backend="sycl",
        device=0,
        output=tmp_path / "out.json",
    )
    assert "--sycl_device" in cmd


def test_build_command_hip_includes_hip_device(tmp_path: Path) -> None:
    cmd = build_command(
        binary=tmp_path / "vmaf",
        ref=tmp_path / "ref.yuv",
        dist=tmp_path / "dist.yuv",
        width=320,
        height=240,
        pix_fmt="420",
        bitdepth=8,
        feature="float_ssim_lcs",
        backend="hip",
        device=0,
        output=tmp_path / "out.json",
    )
    assert cmd[cmd.index("--hip_device") + 1] == "0"
    assert cmd[cmd.index("--feature") + 1] == "float_ssim_hip=enable_lcs=true"
    assert cmd[cmd.index("--backend") + 1] == "hip"


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


def _make_frame(metrics: dict[str, float | None]) -> dict[str, Any]:
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
    assert _close(per_max["integer_vif_scale0"], 1e-3)
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
    assert _close(per_max["integer_vif_scale0"], 5e-4)
    # All three frames exceed tolerance=5e-5.
    n_frames = len(frames_a)
    assert per_mismatch["integer_vif_scale0"] == n_frames


def test_diff_frames_mismatched_lengths_raises() -> None:
    metrics = ("psnr_y",)
    frames_a = [_make_frame({"psnr_y": 40.0}), _make_frame({"psnr_y": 38.0})]
    frames_b = [_make_frame({"psnr_y": 40.0})]
    with _raises(ValueError):
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
    assert _close(tol, DEFAULT_FP16_TOLERANCE)
    assert src == "fp16"


def test_resolve_cell_tolerance_no_calibration_returns_feature_default() -> None:
    expected = FEATURE_TOLERANCE["vif"]
    tol, src = resolve_cell_tolerance(
        "vif",
        fp16_features=[],
        calibration=None,
        gpu_id=None,
    )
    assert _close(tol, expected)
    assert src == "default"


def test_resolve_cell_tolerance_no_gpu_id_returns_default() -> None:
    table = _calibration_table(("sycl:0x8086:*", {"vif": 1e-6}))
    tol, src = resolve_cell_tolerance(
        "vif",
        fp16_features=[],
        calibration=table,
        gpu_id=None,
    )
    assert _close(tol, FEATURE_TOLERANCE["vif"])
    assert src == "default"


def test_resolve_cell_tolerance_calibrated_override() -> None:
    table = _calibration_table(("sycl:0x8086:*", {"vif": 1.5e-5}))
    tol, src = resolve_cell_tolerance(
        "vif",
        fp16_features=[],
        calibration=table,
        gpu_id="sycl:0x8086:0x56a5",
    )
    assert _close(tol, 1.5e-5)
    assert "calibrated" in src
    assert "sycl:0x8086:*" in src


def test_resolve_cell_tolerance_no_match_returns_no_calibration_label() -> None:
    table = _calibration_table(("cuda:8.6", {"vif": 1e-6}))
    tol, src = resolve_cell_tolerance(
        "vif",
        fp16_features=[],
        calibration=table,
        gpu_id="sycl:0x8086:0x56a5",
    )
    assert _close(tol, FEATURE_TOLERANCE["vif"])
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
    assert _close(tol, FEATURE_TOLERANCE["vif"])
    assert "placeholder" in src


def test_resolve_cell_tolerance_unknown_feature_uses_fp32_default() -> None:
    tol, src = resolve_cell_tolerance(
        "nonexistent_feature",
        fp16_features=[],
        calibration=None,
        gpu_id=None,
    )
    assert _close(tol, DEFAULT_FP32_TOLERANCE)
    assert src == "default"


def test_resolve_cell_tolerance_calibrated_with_feature_override_uses_override() -> None:
    table = _calibration_table(("cuda:8.6", {"ciede": 3.0e-3}))
    tol, src = resolve_cell_tolerance(
        "ciede",
        fp16_features=[],
        calibration=table,
        gpu_id="cuda:8.6",
    )
    assert _close(tol, 3.0e-3)
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
        tolerance_source="calibrated:sycl:0x8086:*",
    )
    out = tmp_path / "out.md"
    emit_md([result], out)
    text = out.read_text(encoding="utf-8")
    assert "calibrated:sycl:0x8086:*" in text


# ---------------------------------------------------------------------------
# FEATURE_METRICS completeness check
# ---------------------------------------------------------------------------


def test_every_feature_in_feature_tolerance_is_in_feature_metrics() -> None:
    """All keys in FEATURE_TOLERANCE must appear in FEATURE_METRICS."""
    missing = set(FEATURE_TOLERANCE) - set(FEATURE_METRICS)
    message = f"Features in FEATURE_TOLERANCE but missing from FEATURE_METRICS: {missing}"
    assert missing == set(), message


def test_feature_metrics_values_are_non_empty_tuples() -> None:
    for feature, metrics in FEATURE_METRICS.items():
        assert isinstance(metrics, tuple), f"{feature}: FEATURE_METRICS value must be a tuple"
        assert len(metrics) >= 1, f"{feature}: FEATURE_METRICS must have at least one metric name"


# ---------------------------------------------------------------------------
# ADR-1361: area-scaled psnr_hvs tolerance
# ---------------------------------------------------------------------------

# Worst per-frame psnr_hvs_y difference between CPU and SYCL on 50 frames of
# BBB 3840x2160, measured on an Arc B580 and a UHD 770 (Research-2123).
_MEASURED_4K_PSNR_HVS_Y = 8.423e-4


def _psnr_hvs_tolerance(width: int, height: int) -> float:
    tol, _ = resolve_cell_tolerance(
        "psnr_hvs",
        fp16_features=[],
        calibration=None,
        gpu_id=None,
        width=width,
        height=height,
    )
    return tol


_TERMS_PER_BLOCK = 64
# Anchor of the stated bound: T_ref / ((10 / ln 10) * 2**-24 * sqrt(N_ref)) = 3.93.
_LAMBDA_RANGE = (3.9, 4.0)


def test_psnr_hvs_term_count_matches_calc_psnrhvs_loop() -> None:
    # for (y = 0; y < h - 7; y += 7) / for (x = 0; x < w - 7; x += 7), 64 terms a block.
    assert psnr_hvs_term_count(576, 324) == _TERMS_PER_BLOCK * 82 * 46
    assert psnr_hvs_term_count(3840, 2160) == _TERMS_PER_BLOCK * 548 * 308
    assert psnr_hvs_term_count(8, 8) == _TERMS_PER_BLOCK
    assert psnr_hvs_term_count(15, 8) == 2 * _TERMS_PER_BLOCK
    assert psnr_hvs_term_count(7, 64) == 0


def test_psnr_hvs_tolerance_unchanged_at_reference_geometry() -> None:
    tol, src = resolve_cell_tolerance(
        "psnr_hvs", fp16_features=[], calibration=None, gpu_id=None, width=576, height=324
    )
    assert _close(tol, FEATURE_TOLERANCE["psnr_hvs"])
    assert src == "default"


def test_psnr_hvs_tolerance_never_loosens_or_tightens_below_reference() -> None:
    for width, height in ((575, 324), (576, 323), (256, 144), (8, 8), (7, 7), (576, 8)):
        tolerance = _psnr_hvs_tolerance(width, height)
        assert _close(tolerance, FEATURE_TOLERANCE["psnr_hvs"]), (width, height)


def test_psnr_hvs_tolerance_first_step_above_reference_scales() -> None:
    # One more block column (583 = 576 + 7) is the first width above N_ref.
    expected = FEATURE_TOLERANCE["psnr_hvs"] * math.sqrt(83 / 82)
    assert _close(_psnr_hvs_tolerance(583, 324), expected)


def test_psnr_hvs_tolerance_grows_with_sqrt_of_term_count() -> None:
    tol_1080 = _psnr_hvs_tolerance(1920, 1080)
    tol_4k = _psnr_hvs_tolerance(3840, 2160)
    tol_8k = _psnr_hvs_tolerance(7680, 4320)
    assert FEATURE_TOLERANCE["psnr_hvs"] < tol_1080 < tol_4k < tol_8k
    ratio = tol_4k / FEATURE_TOLERANCE["psnr_hvs"]
    assert _close(ratio, math.sqrt(psnr_hvs_term_count(3840, 2160) / (64 * 82 * 46)))


def test_psnr_hvs_tolerance_matches_stated_accumulation_bound() -> None:
    # T(N) = max(T_ref, (10 / ln 10) * lam * u * sqrt(N)), lam anchored at 576x324.
    u = 2.0**-24
    db_per_relative = 10.0 / math.log(10.0)
    n_ref = psnr_hvs_term_count(576, 324)
    lam = FEATURE_TOLERANCE["psnr_hvs"] / (db_per_relative * u * math.sqrt(n_ref))
    assert _LAMBDA_RANGE[0] < lam < _LAMBDA_RANGE[1]
    n_4k = psnr_hvs_term_count(3840, 2160)
    bound = max(FEATURE_TOLERANCE["psnr_hvs"], db_per_relative * lam * u * math.sqrt(n_4k))
    assert _close(_psnr_hvs_tolerance(3840, 2160), bound)


def test_psnr_hvs_4k_measured_difference_passes_and_wrong_value_fails() -> None:
    tolerance = _psnr_hvs_tolerance(3840, 2160)
    metrics = FEATURE_METRICS["psnr_hvs"]
    reference = [_make_frame(dict.fromkeys(metrics, 40.0))]
    measured = [_make_frame(dict.fromkeys(metrics, 40.0 + _MEASURED_4K_PSNR_HVS_Y))]
    wrong = [_make_frame(dict.fromkeys(metrics, 40.0 + 1e-2))]
    _, ok_mismatch = diff_frames(reference, measured, metrics, tolerance)
    _, bad_mismatch = diff_frames(reference, wrong, metrics, tolerance)
    assert all(count == 0 for count in ok_mismatch.values())
    assert all(count == 1 for count in bad_mismatch.values())
    # The unscaled contract is what used to fail this fixture.
    _, old_mismatch = diff_frames(reference, measured, metrics, FEATURE_TOLERANCE["psnr_hvs"])
    assert all(count == 1 for count in old_mismatch.values())


def test_area_scaling_applies_only_to_psnr_hvs() -> None:
    for feature in ("vif", "ciede", "ssimulacra2", "float_ssim"):
        tol, src = resolve_cell_tolerance(
            feature, fp16_features=[], calibration=None, gpu_id=None, width=3840, height=2160
        )
        assert _close(tol, FEATURE_TOLERANCE[feature]), feature
        assert "area" not in src
    assert area_tolerance_factor("psnr_hvs", None, None) == 1.0


def test_area_scaling_multiplies_a_calibrated_psnr_hvs_row() -> None:
    table = _calibration_table(("sycl:0x8086:*", {"psnr_hvs": 2e-4}))
    tol, src = resolve_cell_tolerance(
        "psnr_hvs",
        fp16_features=[],
        calibration=table,
        gpu_id="sycl:0x8086:0xe20b",
        width=3840,
        height=2160,
    )
    assert _close(tol, 2e-4 * area_tolerance_factor("psnr_hvs", 3840, 2160))
    assert src.startswith("calibrated:sycl:0x8086:*+area x")


def test_diff_frames_null_metrics_agree_only_when_both_null() -> None:
    # vmaf writes an infinite psnr_hvs_cb (identical chroma) as JSON null.
    metrics = ("psnr_hvs_cb",)
    both_null = [_make_frame({"psnr_hvs_cb": None})]
    finite = [_make_frame({"psnr_hvs_cb": 50.0})]
    per_max, per_mismatch = diff_frames(both_null, both_null, metrics, tolerance=5e-4)
    assert per_max["psnr_hvs_cb"] == 0.0
    assert per_mismatch["psnr_hvs_cb"] == 0
    per_max, per_mismatch = diff_frames(both_null, finite, metrics, tolerance=5e-4)
    assert math.isinf(per_max["psnr_hvs_cb"])
    assert per_mismatch["psnr_hvs_cb"] == 1


def test_area_scaling_leaves_fp16_contract_absolute() -> None:
    tol, src = resolve_cell_tolerance(
        "psnr_hvs",
        fp16_features=["psnr_hvs"],
        calibration=None,
        gpu_id=None,
        width=3840,
        height=2160,
    )
    assert _close(tol, DEFAULT_FP16_TOLERANCE)
    assert src == "fp16"


# ---------------------------------------------------------------------------
# ADR-1397: twins that return the CPU's bits are compared exactly
# ---------------------------------------------------------------------------

# Smallest CPU-vs-CUDA psnr_hvs difference a per-block sum left on the Netflix
# 576x324 pair before ADR-1397 (psnr_hvs_cb), and the 3840x2160 one that broke
# the ADR-1361 tolerance (psnr_hvs_y, 24 BBB frames).
_PRE_1397_CUDA_576 = 2.861e-5
_PRE_1397_CUDA_4K = 1.099e-2
# The same two for the SYCL and HIP twins before ADR-1401 (Arc A380, gfx1036),
# and what a float product and root in the masking threshold alone leaves on
# the 576x324 pair once the sum is the CPU's (psnr_hvs_cr).
_PRE_1401_TWIN_576 = 2.893e-5
_PRE_1401_TWIN_4K = 1.099e-2
_FLOAT_THRESHOLD_576 = 4.374e-7
# One unit in the last place of a 30 dB score: what a different host log10
# moves (glibc against Intel's libimf), and so what two binaries may differ by.
_ONE_ULP_30_DB = 3.553e-15


def _psnr_hvs_cell(backend_a: str, backend_b: str, width: int, height: int) -> tuple[float, str]:
    return resolve_cell_tolerance(
        "psnr_hvs",
        fp16_features=[],
        calibration=None,
        gpu_id=None,
        width=width,
        height=height,
        backends=(backend_a, backend_b),
    )


def test_ciede_cuda_cell_is_bounded_by_its_math_library_and_other_twins_are_not() -> None:
    """ADR-1426, ADR-1436, ADR-1448: three twins run the CPU's arithmetic."""

    def cell(backend_a: str, backend_b: str) -> tuple[float, str]:
        return resolve_cell_tolerance(
            "ciede",
            fp16_features=[],
            calibration=None,
            gpu_id=None,
            width=3840,
            height=2160,
            backends=(backend_a, backend_b),
        )

    assert LIBM_TWINS["ciede"] == {"cuda": 1e-9, "sycl": 1e-9, "hip": 1e-9}
    for backend in ("cuda", "sycl", "hip"):
        assert not is_exact_pair("ciede", "cpu", backend)
        assert libm_pair_tolerance("ciede", "cpu", backend) == LIBM_TWINS["ciede"][backend]
    # A twin that is not listed keeps the feature's default.
    assert libm_pair_tolerance("ciede", "cpu", "metal") is None
    assert libm_pair_tolerance("ciede", "cpu", "cpu") is None
    assert libm_pair_tolerance("vif", "cpu", "cuda") is None
    listed = (("cpu", "cuda"), ("cuda", "cpu"), ("cpu", "sycl"), ("cuda", "sycl"), ("cpu", "hip"))
    for pair in (*listed, ("hip", "cpu"), ("cuda", "hip"), ("sycl", "hip")):
        assert cell(*pair) == (1e-9, LIBM_TWIN_SOURCE), pair
    for pair in (("cpu", "metal"), ("cuda", "metal"), ("hip", "metal")):
        tolerance, source = cell(*pair)
        assert _close(tolerance, FEATURE_TOLERANCE["ciede"]), pair
        assert source == "default", pair
    # The fp16 opt-in still wins, as it does over an exact cell.
    assert resolve_cell_tolerance(
        "ciede",
        fp16_features=["ciede"],
        calibration=None,
        gpu_id=None,
        backends=("cpu", "cuda"),
    ) == (DEFAULT_FP16_TOLERANCE, "fp16")


def test_speed_cells_are_exact_since_the_twins_use_the_host_log2() -> None:
    """ADR-1477: speed.c is Netflix's fp64 form and the twins' logarithms are the host's.

    Until then the fork's speed.c called `log2f`, each twin rounded log2 on
    the device, and the six cells were `LIBM_TWINS` bounds (ADR-1430, ADR-1452,
    ADR-1460: 5e-6 for speed_chroma, 4e-5 for speed_temporal).
    """

    def cell(feature: str, backend_a: str, backend_b: str) -> tuple[float, str]:
        return resolve_cell_tolerance(
            feature,
            fp16_features=[],
            calibration=None,
            gpu_id=None,
            width=3840,
            height=2160,
            backends=(backend_a, backend_b),
        )

    assert FEATURE_METRICS["speed_chroma"] == (
        "speed_chroma_u",
        "speed_chroma_v",
        "speed_chroma_uv",
    )
    assert FEATURE_METRICS["speed_temporal"] == ("speed_temporal",)
    for feature in ("speed_chroma", "speed_temporal"):
        # No bound is left: a libm entry would loosen an exact cell.
        assert feature not in LIBM_TWINS
        assert EXACT_TWINS[feature] == frozenset({"cuda", "hip", "sycl"})
        for backend in ("cuda", "hip", "sycl"):
            assert is_exact_pair(feature, "cpu", backend)
            assert cell(feature, "cpu", backend) == (0.0, EXACT_TWIN_SOURCE)
            assert cell(feature, backend, "cpu") == (0.0, EXACT_TWIN_SOURCE)
            assert libm_pair_tolerance(feature, "cpu", backend) is None
            assert feature_extractor_name(feature, backend) == f"{feature}_{backend}"
        # Two exact twins are exact against each other.
        assert cell(feature, "cuda", "hip") == (0.0, EXACT_TWIN_SOURCE)
        assert cell(feature, "hip", "sycl") == (0.0, EXACT_TWIN_SOURCE)
        assert feature_extractor_name(feature, "cpu") == feature
    # The differences the old bounds admitted fail an exact cell: the largest
    # a glibc CPU had from a twin on speed_chroma (BBB frame 21, 1.431e-6) and
    # one float step of a speed_temporal score below 8 (BBB frames 100, 102).
    for feature, measured in (("speed_chroma", 1.431e-6), ("speed_temporal", 4.768e-7)):
        metrics = FEATURE_METRICS[feature]
        tolerance, _ = cell(feature, "cpu", "cuda")
        reference = [_make_frame(dict.fromkeys(metrics, 6.5))]
        drifted = [_make_frame(dict.fromkeys(metrics, 6.5 + measured))]
        _, mismatches = diff_frames(reference, drifted, metrics, tolerance)
        assert all(count == 1 for count in mismatches.values()), (feature, measured)


def test_psnr_cell_compares_all_three_planes() -> None:
    """ADR-1460: the matrix gate compared psnr_y only; the twins emit three planes."""
    assert FEATURE_METRICS["psnr"] == ("psnr_y", "psnr_cb", "psnr_cr")


def test_psnr_hvs_per_block_twin_keeps_area_scaled_tolerance() -> None:
    # ADR-1361 stays the contract of a twin outside EXACT_TWINS, and of a
    # caller that names no backends.
    for pair in (("cpu", "metal"), ("cuda", "metal")):
        tolerance, source = _psnr_hvs_cell(*pair, 576, 324)
        assert _close(tolerance, FEATURE_TOLERANCE["psnr_hvs"]), pair
        assert source == "default"
        tolerance, source = _psnr_hvs_cell(*pair, 3840, 2160)
        assert _close(tolerance, _psnr_hvs_tolerance(3840, 2160)), pair
        assert "+area x" in source
    tolerance, source = resolve_cell_tolerance(
        "psnr_hvs", fp16_features=[], calibration=None, gpu_id=None, width=3840, height=2160
    )
    assert _close(tolerance, _psnr_hvs_tolerance(3840, 2160))
    assert "+area x" in source


# ---------------------------------------------------------------------------
# ADR-1428: the exact twins come from scripts/ci/exact_twins.d/. These tests
# hold for any set of fragments; none names a feature or backend.
# ---------------------------------------------------------------------------

_GATE_BACKENDS = tuple(backend for backend in BACKEND_SUFFIX if backend != "cpu")
# A backend outside the gate's list: never exact, whatever the fragments say.
_OFF_GATE_BACKEND = "metal"


def _twin_cell(
    feature: str,
    backend_a: str,
    backend_b: str,
    *,
    width: int = 3840,
    height: int = 2160,
    fp16_features: list[str] | None = None,
    calibration: CalibrationTable | None = None,
    gpu_id: str | None = None,
) -> tuple[float, str]:
    return resolve_cell_tolerance(
        feature,
        fp16_features=fp16_features or [],
        calibration=calibration,
        gpu_id=gpu_id,
        width=width,
        height=height,
        backends=(backend_a, backend_b),
    )


def _exact_pairs() -> list[tuple[str, str, str]]:
    """Every (feature, a, b) cell whose two sides are cpu or listed twins."""

    pairs: list[tuple[str, str, str]] = []
    for feature, backends in EXACT_TWINS.items():
        sides = ["cpu", *sorted(backends)]
        pairs.extend((feature, a, b) for a in sides for b in sides if a != b)
    return pairs


def test_every_fragment_is_valid() -> None:
    assert EXACT_TWIN_FRAGMENTS
    validate_exact_twins(EXACT_TWIN_FRAGMENTS, FEATURE_METRICS, BACKEND_SUFFIX)
    for twin in EXACT_TWIN_FRAGMENTS:
        assert twin.feature in FEATURE_METRICS, twin
        assert twin.backend in BACKEND_SUFFIX and twin.backend != "cpu", twin
        assert twin.adrs and twin.evidence, twin
        for adr in twin.adrs:
            assert list(ADR_DIR.glob(f"{adr[4:]}-*.md")), (twin, adr)
        assert (EXACT_TWINS_DIR / f"{twin.feature}.{twin.backend}").is_file()


def test_exact_twins_is_the_sorted_grouping_of_the_fragments() -> None:
    assert build_exact_twins(EXACT_TWIN_FRAGMENTS) == EXACT_TWINS
    assert list(EXACT_TWINS) == sorted(EXACT_TWINS)
    keys = [(twin.feature, twin.backend) for twin in EXACT_TWIN_FRAGMENTS]
    assert keys == sorted(keys)
    assert load_exact_twin_fragments() == EXACT_TWIN_FRAGMENTS


def test_exact_pair_is_cpu_and_listed_twins_only() -> None:
    for feature, backends in EXACT_TWINS.items():
        for backend in backends:
            assert is_exact_pair(feature, "cpu", backend), (feature, backend)
            assert is_exact_pair(feature, backend, "cpu"), (feature, backend)
            # A side that is not listed spoils the cell, whichever side it is.
            assert not is_exact_pair(feature, backend, _OFF_GATE_BACKEND), (feature, backend)
            assert not is_exact_pair(feature, _OFF_GATE_BACKEND, backend), (feature, backend)
        assert not is_exact_pair(feature, "cpu", _OFF_GATE_BACKEND), feature
        for a in backends:
            for b in backends:
                assert is_exact_pair(feature, a, b), (feature, a, b)
        for backend in set(_GATE_BACKENDS) - backends:
            assert not is_exact_pair(feature, "cpu", backend), (feature, backend)
    # A feature without a fragment is never exact, whatever the backends.
    for feature in set(FEATURE_METRICS) - set(EXACT_TWINS):
        for backend in (*_GATE_BACKENDS, _OFF_GATE_BACKEND):
            assert not is_exact_pair(feature, "cpu", backend), (feature, backend)
    assert not is_exact_pair("no_such_feature", "cpu", "cuda")


def test_exact_cells_are_exact_at_every_size() -> None:
    for feature, a, b in _exact_pairs():
        for width, height in ((8, 8), (576, 324), (1920, 1080), (3840, 2160), (7680, 4320)):
            cell = _twin_cell(feature, a, b, width=width, height=height)
            assert cell == (0.0, EXACT_TWIN_SOURCE), (feature, a, b, width, height)


def test_exact_cell_ignores_a_calibration_row() -> None:
    for feature, a, b in _exact_pairs():
        table = _calibration_table(("cuda:8.9", {feature: 5e-4}))
        cell = _twin_cell(feature, a, b, calibration=table, gpu_id="cuda:8.9")
        assert cell == (0.0, EXACT_TWIN_SOURCE), (feature, a, b)


def test_exact_cell_yields_to_an_explicit_fp16_contract() -> None:
    for feature, a, b in _exact_pairs():
        tolerance, source = _twin_cell(feature, a, b, fp16_features=[feature])
        assert _close(tolerance, DEFAULT_FP16_TOLERANCE), (feature, a, b)
        assert source == "fp16", (feature, a, b)


def test_unlisted_backend_cells_keep_their_tolerance_contract() -> None:
    exact = set(_exact_pairs())
    sides = ("cpu", *_GATE_BACKENDS)
    for feature in FEATURE_METRICS:
        for a in sides:
            for b in sides:
                if a == b or (feature, a, b) in exact:
                    continue
                tolerance, source = _twin_cell(feature, a, b)
                assert source != EXACT_TWIN_SOURCE, (feature, a, b)
                if libm_pair_tolerance(feature, a, b) is not None:
                    # A math-library twin (ADR-1426) has its own bound and test.
                    assert source == LIBM_TWIN_SOURCE, (feature, a, b)
                    continue
                # The same tolerance a caller naming no backends gets.
                plain = resolve_cell_tolerance(
                    feature,
                    fp16_features=[],
                    calibration=None,
                    gpu_id=None,
                    width=3840,
                    height=2160,
                )
                assert (tolerance, source) == plain, (feature, a, b)


def test_exact_cell_fails_the_differences_a_per_block_sum_left() -> None:
    deltas = (
        _PRE_1397_CUDA_576,
        _PRE_1397_CUDA_4K,
        _PRE_1401_TWIN_576,
        _PRE_1401_TWIN_4K,
        _FLOAT_THRESHOLD_576,
        2.0**-45,
        _ONE_ULP_30_DB,
    )
    for feature, a, b in _exact_pairs():
        metrics = FEATURE_METRICS[feature]
        tolerance, _ = _twin_cell(feature, a, b)
        reference = [_make_frame(dict.fromkeys(metrics, 30.0))]
        for delta in deltas:
            drifted = [_make_frame(dict.fromkeys(metrics, 30.0 + delta))]
            _, mismatches = diff_frames(reference, drifted, metrics, tolerance)
            assert all(count == 1 for count in mismatches.values()), (feature, a, b, delta)
        _, mismatches = diff_frames(reference, reference, metrics, tolerance)
        assert all(count == 0 for count in mismatches.values()), (feature, a, b)


def test_exact_cell_runs_both_sides_at_full_precision(tmp_path: Path) -> None:
    def command(feature: str, backend: str, precision: str | None) -> list[str]:
        return build_command(
            binary=tmp_path / "vmaf",
            ref=tmp_path / "ref.yuv",
            dist=tmp_path / "dist.yuv",
            width=3840,
            height=2160,
            pix_fmt="420",
            bitdepth=8,
            feature=feature,
            backend=backend,
            device=None if backend == "cpu" else 1,
            output=tmp_path / "out.json",
            precision=precision,
        )

    for feature, backends in EXACT_TWINS.items():
        for backend in ("cpu", *backends):
            cmd = command(feature, backend, "max")
            assert cmd[cmd.index("--precision") + 1] == "max"
            # A tolerance cell keeps the CLI's default output precision.
            assert "--precision" not in command(feature, backend, None)


# -- the loader ---------------------------------------------------------------


def _adr_dir(tmp_path: Path) -> Path:
    adr_dir = tmp_path / "adr"
    adr_dir.mkdir()
    (adr_dir / "0001-fixture.md").write_text("# ADR-0001\n", encoding="utf-8")
    return adr_dir


def _fragments(tmp_path: Path, files: dict[str, str]) -> Path:
    directory = tmp_path / "exact_twins.d"
    directory.mkdir()
    for name, text in files.items():
        (directory / name).write_text(text, encoding="utf-8")
    return directory


# The generator's exit status for an unreadable fragment set (sysexits EX_DATAERR).
_EX_DATAERR = 65
_GOOD = "adr: ADR-0001\nevidence: fixtures and result\n"


def test_loader_reads_sorted_fragments_from_a_directory(tmp_path: Path) -> None:
    directory = _fragments(
        tmp_path,
        {"vif.sycl": _GOOD, "adm.hip": _GOOD, "adm.cuda": "adr: ADR-0001, ADR-0001\nevidence: e\n"},
    )
    twins = load_exact_twin_fragments(directory, _adr_dir(tmp_path))
    assert [(twin.feature, twin.backend) for twin in twins] == [
        ("adm", "cuda"),
        ("adm", "hip"),
        ("vif", "sycl"),
    ]
    assert twins[0].adrs == ("ADR-0001", "ADR-0001")
    assert twins[1].evidence == "fixtures and result"
    assert build_exact_twins(twins) == {
        "adm": frozenset({"cuda", "hip"}),
        "vif": frozenset({"sycl"}),
    }


def test_loader_rejects_a_missing_or_empty_directory(tmp_path: Path) -> None:
    adr_dir = _adr_dir(tmp_path)
    with _raises(ExactTwinError):
        load_exact_twin_fragments(tmp_path / "absent", adr_dir)
    empty = tmp_path / "empty"
    empty.mkdir()
    with _raises(ExactTwinError):
        load_exact_twin_fragments(empty, adr_dir)


def test_loader_rejects_each_malformed_fragment(tmp_path: Path) -> None:
    adr_dir = _adr_dir(tmp_path)
    bad = {
        "empty file": ("adm.cuda", ""),
        "blank file": ("adm.cuda", "\n  \n"),
        "unknown key": ("adm.cuda", _GOOD + "note: x\n"),
        "missing adr": ("adm.cuda", "evidence: e\n"),
        "missing evidence": ("adm.cuda", "adr: ADR-0001\n"),
        "duplicate key": ("adm.cuda", _GOOD + "adr: ADR-0001\n"),
        "no colon": ("adm.cuda", "adr ADR-0001\nevidence: e\n"),
        "empty value": ("adm.cuda", "adr:\nevidence: e\n"),
        "adr not a reference": ("adm.cuda", "adr: 1397\nevidence: e\n"),
        "adr without a file": ("adm.cuda", f"adr: ADR-{'9' * 4}\nevidence: e\n"),
        "name without backend": ("adm", _GOOD),
        "name with a suffix": ("adm.cuda.txt", _GOOD),
        "cpu is not a twin": ("adm.cpu", _GOOD),
        "upper case name": ("ADM.cuda", _GOOD),
    }
    for label, (name, text) in bad.items():
        case = tmp_path / label.replace(" ", "_")
        case.mkdir()
        directory = _fragments(case, {name: text})
        with _raises(ExactTwinError):
            load_exact_twin_fragments(directory, adr_dir)


def test_loader_rejects_a_directory_entry(tmp_path: Path) -> None:
    directory = _fragments(tmp_path, {"adm.cuda": _GOOD})
    (directory / "sub.dir").mkdir()
    with _raises(ExactTwinError):
        load_exact_twin_fragments(directory, _adr_dir(tmp_path))


def test_validate_rejects_an_unknown_feature_or_backend(tmp_path: Path) -> None:
    directory = _fragments(tmp_path, {"adm.cuda": _GOOD})
    twins = load_exact_twin_fragments(directory, _adr_dir(tmp_path))
    validate_exact_twins(twins, {"adm"}, {"cuda"})
    with _raises(ExactTwinError):
        validate_exact_twins(twins, {"vif"}, {"cuda"})
    with _raises(ExactTwinError):
        validate_exact_twins(twins, {"adm"}, {"sycl"})


def test_generated_twin_table_is_current_and_follows_the_fragments(tmp_path: Path) -> None:
    script = Path(__file__).resolve().parents[1] / "docs" / "generate-exact-twins.py"
    directory = _fragments(tmp_path, {"adm.cuda": _GOOD})
    output = tmp_path / "table.md"
    args = ["--fragments", str(directory), "--adr-dir", str(_adr_dir(tmp_path)), "--output"]

    def run(*extra: str) -> int:
        # ADR-1242: the fixed repository generator on a disposable fixture, no shell.
        result = run_command(
            (sys.executable, str(script), *extra, *args, str(output)),
            allowed_executables=(sys.executable,),
            text=True,
            capture_output=True,
            check=False,
            timeout_seconds=60,
        )
        return result.returncode

    assert run("--check") == 1  # nothing rendered yet
    assert run("--write") == 0
    assert run("--check") == 0
    assert "| `adm` | `cuda` | [ADR-0001](../adr/0001-fixture.md) | fixtures and result |" in (
        output.read_text(encoding="utf-8")
    )
    (directory / "vif.hip").write_text(_GOOD, encoding="utf-8")
    assert run("--check") == 1  # a new fragment makes the file stale
    (directory / "vif.hip").write_text("evidence: e\n", encoding="utf-8")
    assert run("--check") == _EX_DATAERR  # a malformed fragment fails loudly


def test_feature_metrics_motion_reads_default_emitted_keys() -> None:
    assert FEATURE_METRICS["motion"] == (
        "VMAF_integer_feature_motion_sad_score",
        "integer_motion2",
        "integer_motion3",
    )


# ---------------------------------------------------------------------------
# ADR-1418: motion cells and metrics a backend does not emit
# ---------------------------------------------------------------------------


def test_motion_and_motion_debug_feature_names() -> None:
    assert feature_extractor_name("motion", "cpu") == "motion"
    assert feature_extractor_name("motion", "cuda") == "motion_cuda"
    assert feature_extractor_name("motion", "sycl") == "motion_sycl"
    assert feature_extractor_name("motion", "hip") == "motion_hip"
    assert feature_extractor_name("motion_debug", "cpu") == "motion=debug=true"
    assert feature_extractor_name("motion_debug", "cuda") == "motion_cuda=debug=true"
    assert feature_extractor_name("motion_debug", "sycl") == "motion_sycl=debug=true"
    assert feature_extractor_name("motion_debug", "hip") == "motion_hip=debug=true"


def test_motion_feature_metrics_definitions() -> None:
    # The CPU appends the SAD score on every frame, with and without debug
    # (integer_motion.c::extract); both cells compare it.
    assert FEATURE_METRICS["motion"] == (
        "VMAF_integer_feature_motion_sad_score",
        "integer_motion2",
        "integer_motion3",
    )
    assert FEATURE_METRICS["motion_debug"] == (
        "VMAF_integer_feature_motion_sad_score",
        "integer_motion",
        "integer_motion2",
        "integer_motion3",
    )
    assert FEATURE_TOLERANCE["motion_debug"] == FEATURE_TOLERANCE["motion"]


def test_five_frame_window_cells() -> None:
    # ADR-1491: the five-frame window with the moving average, the option
    # set of the vmaf_v1.0.16_hfr_* models, on `motion` and `motion_v2`.
    options = "motion_five_frame_window=true:motion_moving_average=true"
    assert feature_extractor_name("motion_mffw", "cpu") == f"motion={options}"
    assert feature_extractor_name("motion_mffw", "cuda") == f"motion_cuda={options}"
    assert feature_extractor_name("motion_v2_mffw", "sycl") == f"motion_v2_sycl={options}"
    assert feature_extractor_name("motion_v2_mffw", "hip") == f"motion_v2_hip={options}"
    # The names carry the option aliases in option-name order.
    assert FEATURE_METRICS["motion_mffw"] == (
        "VMAF_integer_feature_motion_sad_score_mffw_mma",
        "integer_motion2_mffw_mma",
        "integer_motion3_mffw_mma",
    )
    assert FEATURE_METRICS["motion_v2_mffw"] == (
        "VMAF_integer_feature_motion_v2_sad_score_mffw_mma",
        "VMAF_integer_feature_motion2_v2_score_mffw_mma",
        "VMAF_integer_feature_motion3_v2_score_mffw_mma",
    )
    for feature in ("motion_mffw", "motion_v2_mffw"):
        assert FEATURE_TOLERANCE[feature] == FEATURE_TOLERANCE["motion"]
        for backend in ("cuda", "sycl", "hip"):
            assert is_exact_pair(feature, "cpu", backend), (feature, backend)


def test_missing_metrics_names_what_any_frame_lacks() -> None:
    metrics = ("integer_motion", "integer_motion2", "integer_motion3")
    full = _make_frame({"integer_motion": 0.5, "integer_motion2": 1.0, "integer_motion3": 2.0})
    short = _make_frame({"integer_motion2": 1.0, "integer_motion3": 2.0})
    assert missing_metrics([full, full], metrics) == []
    assert missing_metrics([short], metrics) == ["integer_motion"]
    # One frame without the metric is enough: a twin must emit it on every frame.
    assert missing_metrics([full, short], metrics) == ["integer_motion"]
    assert missing_metrics([{"frameNum": 0}], metrics) == list(metrics)


_MOTION_SAD = "VMAF_integer_feature_motion_sad_score"


def _run_motion_debug_cell(
    tmp_path: Path, monkeypatch: Any, metrics_by_backend: dict[str, dict[str, float]]
) -> CellResult:
    def fake_run_one(
        binary: Path,
        ref: Path,
        dist: Path,
        width: int,
        height: int,
        pix_fmt: str,
        bitdepth: int,
        feature: str,
        backend: str,
        device: int | None,
        output: Path,
        precision: str | None = None,
    ) -> tuple[int, str]:
        data = {"frames": [{"frameNum": 0, "metrics": metrics_by_backend[backend]}]}
        output.write_text(json.dumps(data), encoding="utf-8")
        return 0, ""

    monkeypatch.setattr("scripts.ci.cross_backend_parity_gate.run_one", fake_run_one)
    return run_cell(
        Cell(feature="motion_debug", backend_a="cpu", backend_b="sycl"),
        binary=tmp_path / "vmaf",
        ref=tmp_path / "ref.yuv",
        dist=tmp_path / "dist.yuv",
        width=576,
        height=324,
        pix_fmt="420",
        bitdepth=8,
        workdir=tmp_path,
        devices={},
        tolerance=5e-5,
    )


def test_run_cell_reports_a_metric_one_backend_lacks_as_error(
    tmp_path: Path, monkeypatch: Any
) -> None:
    """A twin that drops a metric must not pass, and must not stop the matrix."""

    result = _run_motion_debug_cell(
        tmp_path,
        monkeypatch,
        {
            "cpu": {_MOTION_SAD: 0.75, "integer_motion2": 1.5, "integer_motion3": 2.5},
            "sycl": {
                _MOTION_SAD: 0.75,
                "integer_motion": 0.75,
                "integer_motion2": 1.5,
                "integer_motion3": 2.5,
            },
        },
    )
    assert result.status == "ERROR"
    assert "backend_a cpu lacks ['integer_motion']" in result.note
    assert "backend_b sycl lacks []" in result.note


def test_run_cell_reports_a_twin_without_the_sad_score_as_error(
    tmp_path: Path, monkeypatch: Any
) -> None:
    """motion_sycl before T-GPU-MOTION-SAD-SCORE-NOT-EMITTED-2026-10-02."""

    emitted = {"integer_motion": 0.75, "integer_motion2": 1.5, "integer_motion3": 2.5}
    result = _run_motion_debug_cell(
        tmp_path,
        monkeypatch,
        {"cpu": dict(emitted, **{_MOTION_SAD: 0.75}), "sycl": emitted},
    )
    assert result.status == "ERROR"
    assert "backend_a cpu lacks []" in result.note
    assert f"backend_b sycl lacks ['{_MOTION_SAD}']" in result.note


def test_run_cell_compares_every_metric_when_both_backends_emit_them(
    tmp_path: Path, monkeypatch: Any
) -> None:
    emitted = {
        _MOTION_SAD: 0.75,
        "integer_motion": 0.75,
        "integer_motion2": 1.5,
        "integer_motion3": 2.5,
    }
    result = _run_motion_debug_cell(
        tmp_path,
        monkeypatch,
        {"cpu": emitted, "sycl": dict(emitted, integer_motion=0.76)},
    )
    assert result.status == "FAIL"
    assert result.per_metric_mismatches == {
        _MOTION_SAD: 0,
        "integer_motion": 1,
        "integer_motion2": 0,
        "integer_motion3": 0,
    }


# ---------------------------------------------------------------------------
# ADR-1496: the `metal` backend and `--hold-exact`
# ---------------------------------------------------------------------------


def test_build_command_metal_selects_the_device(tmp_path: Path) -> None:
    cmd = build_command(
        binary=tmp_path / "vmaf",
        ref=tmp_path / "ref.yuv",
        dist=tmp_path / "dist.yuv",
        width=576,
        height=324,
        pix_fmt="420",
        bitdepth=8,
        feature="adm",
        backend="metal",
        device=0,
        output=tmp_path / "out.json",
    )
    assert cmd[cmd.index("--backend") + 1] == "metal"
    assert cmd[cmd.index("--metal_device") + 1] == "0"
    assert cmd[cmd.index("--feature") + 1] == "integer_adm_metal"


def test_metal_twins_named_after_the_cpu_file() -> None:
    expected = {
        "adm": "integer_adm_metal",
        "cambi": "integer_cambi_metal",
        "ciede": "integer_ciede_metal",
        "motion": "integer_motion_metal",
        "motion_debug": "integer_motion_metal=debug=true",
        "psnr": "integer_psnr_metal",
        "psnr_hvs": "integer_psnr_hvs_metal",
        "ssim": "integer_ssim_metal",
        "vif": "integer_vif_metal",
        "float_adm": "float_adm_metal",
        "float_ms_ssim_lcs": "float_ms_ssim_metal=enable_lcs=true",
        "motion_v2": "motion_v2_metal",
    }
    for feature, name in expected.items():
        assert feature_extractor_name(feature, "metal") == name, feature


def _tolerance(feature: str, backends: tuple[str, str], held: tuple[str, ...]) -> tuple[float, str]:
    return resolve_cell_tolerance(
        feature,
        fp16_features=[],
        calibration=None,
        gpu_id=None,
        backends=backends,
        held_exact=held,
    )


def test_held_exact_backend_is_compared_exactly() -> None:
    assert _tolerance("adm", ("cpu", "metal"), ("metal",)) == (0.0, HELD_EXACT_SOURCE)
    assert _tolerance("float_ms_ssim_lcs", ("cpu", "metal"), ("metal",)) == (0.0, HELD_EXACT_SOURCE)


def test_held_exact_math_library_feature_keeps_its_bound() -> None:
    bound = max(LIBM_TWINS["ciede"].values())
    assert _tolerance("ciede", ("cpu", "metal"), ("metal",)) == (bound, LIBM_TWIN_SOURCE)


def test_without_hold_exact_a_metal_cell_keeps_the_feature_tolerance() -> None:
    assert _tolerance("adm", ("cpu", "metal"), ()) == (FEATURE_TOLERANCE["adm"], "default")


def test_hold_exact_needs_every_other_side_exact() -> None:
    # adm is listed for CUDA, so a CUDA <-> held Metal cell is exact; a feature
    # with no CUDA fragment keeps its tolerance in that cell.
    assert "cuda" in EXACT_TWINS["adm"]
    assert _tolerance("adm", ("cuda", "metal"), ("metal",)) == (0.0, HELD_EXACT_SOURCE)
    unlisted = next(f for f in FEATURE_METRICS if "cuda" not in EXACT_TWINS.get(f, frozenset()))
    tolerance, source = _tolerance(unlisted, ("cuda", "metal"), ("metal",))
    assert source != HELD_EXACT_SOURCE
    assert tolerance > 0.0


def test_held_exact_cell_runs_at_full_precision(tmp_path: Path, monkeypatch: Any) -> None:
    seen: list[str | None] = []

    def fake_run_one(*args: Any) -> tuple[int, str]:
        seen.append(args[-1])
        out: Path = args[-2]
        frames = [{"frameNum": 0, "metrics": dict.fromkeys(FEATURE_METRICS["adm"], 0.5)}]
        out.write_text(json.dumps({"frames": frames}))
        return 0, ""

    monkeypatch.setattr(parity_gate, "run_one", fake_run_one)
    result = run_cell(
        Cell("adm", "cpu", "metal"),
        binary=tmp_path / "vmaf",
        ref=tmp_path / "r.yuv",
        dist=tmp_path / "d.yuv",
        width=64,
        height=64,
        pix_fmt="420",
        bitdepth=8,
        workdir=tmp_path,
        devices={"metal": 0},
        tolerance=0.0,
        tolerance_source=HELD_EXACT_SOURCE,
    )
    assert result.status == "OK"
    assert seen == ["max", "max"]
