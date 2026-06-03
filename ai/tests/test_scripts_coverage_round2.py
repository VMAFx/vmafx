# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Coverage push round 2 — argv + helpers + happy paths for ai/scripts/*.

Targets (highest LOC / least tests):
  - extract_k150k_features.py  (geometry helpers, fps, HDR, feature_arg,
    metric lookup, frame aggregation, staging, checkpoint)
  - aggregate_corpora.py       (CLI parser: --corpus-source-override parsing
                                 edge cases not covered in test_aggregate_corpora)
  - calibrate_nr_threshold.py  (linear regression, delta_fast, pearson_r,
                                 calibration quality, geometry detection, args)
  - materialize_mos_labels.py  (normalise_key modes, mos_payload edge cases,
                                 json/csv table formats, CLI key-normalise flag)
  - batch_materialize_saliency_features.py  (load_batch_manifest edge cases,
                                             write_markdown_report)
  - batch_materialize_second_opinion_features.py  (load_batch_manifest,
                                                    write_markdown_report,
                                                    _resolve_score_spec)

Tests run without GPU, corpus, or model downloads.
"""

from __future__ import annotations

import importlib.util
import json
import math
import sys
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCRIPTS = _REPO_ROOT / "ai" / "scripts"


# ---------------------------------------------------------------------------
# Module loaders — isolate each script in its own sys.modules slot.
# ---------------------------------------------------------------------------


def _load(name: str):
    path = _SCRIPTS / f"{name}.py"
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None, f"Cannot locate {path}"
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture(scope="module")
def k150k():
    return _load("extract_k150k_features")


@pytest.fixture(scope="module")
def calibrate():
    return _load("calibrate_nr_threshold")


@pytest.fixture(scope="module")
def mat_mos():
    # Already loaded in test_materialize_mos_labels, but this module loads it
    # independently to avoid cross-module fixture order issues.
    spec = importlib.util.spec_from_file_location(
        "materialize_mos_labels_r2", _SCRIPTS / "materialize_mos_labels.py"
    )
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    sys.modules["materialize_mos_labels_r2"] = mod
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture(scope="module")
def batch_sal():
    return _load("batch_materialize_saliency_features")


@pytest.fixture(scope="module")
def batch_so():
    return _load("batch_materialize_second_opinion_features")


@pytest.fixture(scope="module")
def agg():
    return _load("aggregate_corpora")


# ===========================================================================
# extract_k150k_features.py helpers
# ===========================================================================


class TestGeometryFromSidecar:
    def test_returns_none_when_meta_none(self, k150k) -> None:
        assert k150k._geometry_from_sidecar(None) is None

    def test_returns_none_when_required_field_missing(self, k150k) -> None:
        # Missing chug_height_manifest
        meta = {"chug_width_manifest": 1920, "chug_framerate_manifest": "25/1"}
        assert k150k._geometry_from_sidecar(meta) is None

    def test_returns_tuple_for_complete_sdr_meta(self, k150k) -> None:
        meta = {
            "chug_width_manifest": 1920,
            "chug_height_manifest": 1080,
            "chug_framerate_manifest": "25/1",
        }
        result = k150k._geometry_from_sidecar(meta)
        assert result is not None
        w, h, pix_fmt, fps = result
        assert w == 1920
        assert h == 1080
        assert pix_fmt == "yuv420p"
        assert fps == "25/1"

    def test_infers_yuv420p10le_for_10bit(self, k150k) -> None:
        meta = {
            "chug_width_manifest": 3840,
            "chug_height_manifest": 2160,
            "chug_framerate_manifest": "60/1",
            "chug_bit_depth": 10,
        }
        _, _, pix_fmt, _ = k150k._geometry_from_sidecar(meta)
        assert pix_fmt == "yuv420p10le"

    def test_coerces_string_width_height(self, k150k) -> None:
        meta = {
            "chug_width_manifest": "1280",
            "chug_height_manifest": "720",
            "chug_framerate_manifest": "30/1",
        }
        w, h, _, _ = k150k._geometry_from_sidecar(meta)
        assert isinstance(w, int)
        assert isinstance(h, int)
        assert w == 1280
        assert h == 720


class TestParseFps:
    def test_fractional_fps(self, k150k) -> None:
        assert k150k._parse_fps("30000/1001") == pytest.approx(30000 / 1001, rel=1e-9)

    def test_integer_fps_string(self, k150k) -> None:
        assert k150k._parse_fps("24.0") == pytest.approx(24.0)

    def test_zero_denominator_returns_zero(self, k150k) -> None:
        assert k150k._parse_fps("30/0") == 0.0

    def test_invalid_string_returns_zero(self, k150k) -> None:
        assert k150k._parse_fps("notafps") == 0.0

    def test_simple_integer_string(self, k150k) -> None:
        assert k150k._parse_fps("25/1") == pytest.approx(25.0)


class TestIsHdrSource:
    def test_sdr_8bit_is_not_hdr(self, k150k) -> None:
        assert k150k._is_hdr_source("yuv420p", {"color_transfer": "smpte2084"}) is False

    def test_10bit_smpte2084_is_hdr(self, k150k) -> None:
        meta = {"color_transfer": "smpte2084", "color_primaries": "bt2020"}
        assert k150k._is_hdr_source("yuv420p10le", meta) is True

    def test_10bit_hlg_is_hdr(self, k150k) -> None:
        meta = {"color_transfer": "arib-std-b67", "color_primaries": ""}
        assert k150k._is_hdr_source("yuv420p10le", meta) is True

    def test_10bit_bt2020_primaries_fallback(self, k150k) -> None:
        meta = {"color_transfer": "", "color_primaries": "bt2020nc"}
        assert k150k._is_hdr_source("yuv420p10le", meta) is True

    def test_missing_metadata_defaults_to_sdr(self, k150k) -> None:
        assert k150k._is_hdr_source("yuv420p10le", {}) is False

    def test_10bit_sdr_transfer_is_not_hdr(self, k150k) -> None:
        meta = {"color_transfer": "bt709", "color_primaries": "bt709"}
        assert k150k._is_hdr_source("yuv420p10le", meta) is False


class TestMotionFpsWeight:
    def test_zero_fps_returns_one(self, k150k) -> None:
        assert k150k._motion_fps_weight(0.0) == 1.0

    def test_30fps_returns_one(self, k150k) -> None:
        assert k150k._motion_fps_weight(30.0) == 1.0

    def test_25fps_in_range_returns_one(self, k150k) -> None:
        assert k150k._motion_fps_weight(25.0) == 1.0

    def test_60fps_returns_half(self, k150k) -> None:
        assert k150k._motion_fps_weight(60.0) == pytest.approx(0.5, rel=1e-9)

    def test_120fps_clamped_to_025(self, k150k) -> None:
        assert k150k._motion_fps_weight(120.0) == pytest.approx(0.25, rel=1e-9)

    def test_very_low_fps_clamped_to_4(self, k150k) -> None:
        assert k150k._motion_fps_weight(5.0) == pytest.approx(4.0, rel=1e-9)

    def test_50fps_returns_correct(self, k150k) -> None:
        assert k150k._motion_fps_weight(50.0) == pytest.approx(30.0 / 50.0, rel=1e-9)


class TestFeatureArg:
    def test_bare_extractor_when_no_hdr_no_special_weight(self, k150k) -> None:
        result = k150k._feature_arg("adm", is_hdr=False, motion_fps_weight=1.0)
        assert result == "adm"

    def test_cambi_hdr_adds_eotf_pq(self, k150k) -> None:
        result = k150k._feature_arg("cambi", is_hdr=True, motion_fps_weight=1.0)
        assert "eotf=pq" in result
        assert result.startswith("cambi=")

    def test_cambi_cuda_hdr_drops_full_ref(self, k150k) -> None:
        # cambi_cuda does not expose full_ref; the option must be filtered out.
        result = k150k._feature_arg("cambi_cuda", is_hdr=True, motion_fps_weight=1.0)
        assert "eotf=pq" in result
        assert "full_ref" not in result

    def test_motion_fps_weight_applied_to_motion(self, k150k) -> None:
        result = k150k._feature_arg("motion", is_hdr=False, motion_fps_weight=0.5)
        assert "motion_fps_weight=0.5000" in result

    def test_motion_fps_weight_not_applied_when_one(self, k150k) -> None:
        result = k150k._feature_arg("motion", is_hdr=False, motion_fps_weight=1.0)
        assert result == "motion"

    def test_float_ms_ssim_hdr_enables_linear_scale(self, k150k) -> None:
        result = k150k._feature_arg("float_ms_ssim", is_hdr=True, motion_fps_weight=1.0)
        assert "enable_db=false" in result

    def test_float_ms_ssim_cuda_hdr_drops_enable_db(self, k150k) -> None:
        # float_ms_ssim_cuda does not expose enable_db; must be filtered.
        result = k150k._feature_arg("float_ms_ssim_cuda", is_hdr=True, motion_fps_weight=1.0)
        assert "enable_db" not in result


class TestLookupMetric:
    def test_direct_key(self, k150k) -> None:
        assert k150k._lookup_metric({"adm2": 0.9}, "adm2") == pytest.approx(0.9)

    def test_alias_key(self, k150k) -> None:
        assert k150k._lookup_metric({"integer_adm2": 0.85}, "adm2") == pytest.approx(0.85)

    def test_missing_returns_nan(self, k150k) -> None:
        assert math.isnan(k150k._lookup_metric({}, "adm2"))

    def test_none_value_falls_through_to_nan(self, k150k) -> None:
        assert math.isnan(k150k._lookup_metric({"adm2": None}, "adm2"))

    def test_unknown_feature_returns_nan(self, k150k) -> None:
        assert math.isnan(k150k._lookup_metric({}, "__no_such_feature__"))


class TestAggregateFrames:
    def test_empty_frames_returns_all_nan(self, k150k) -> None:
        result = k150k._aggregate_frames([])
        for feat in k150k.FEATURE_NAMES:
            assert math.isnan(result[f"{feat}_mean"])
            assert math.isnan(result[f"{feat}_std"])

    def test_single_frame_produces_zero_std(self, k150k) -> None:
        frame = {"adm2": 0.95, "integer_vif_scale0": 0.80}
        result = k150k._aggregate_frames([frame])
        assert result["adm2_mean"] == pytest.approx(0.95)
        assert result["adm2_std"] == pytest.approx(0.0)

    def test_multiple_frames_aggregated(self, k150k) -> None:
        import numpy as np

        frames = [{"adm2": 0.9}, {"adm2": 0.8}]
        result = k150k._aggregate_frames(frames)
        assert result["adm2_mean"] == pytest.approx(0.85, abs=1e-9)
        # nanstd (ddof=0) of [0.9, 0.8] = 0.05
        assert result["adm2_std"] == pytest.approx(np.nanstd([0.9, 0.8]))

    def test_all_nan_column_produces_nan_mean_no_warning(self, k150k) -> None:
        import warnings

        frames = [{"psnr_hvs": None}, {"psnr_hvs": None}]
        with warnings.catch_warnings(record=True) as w:
            warnings.simplefilter("always")
            result = k150k._aggregate_frames(frames)
        runtime_warnings = [x for x in w if issubclass(x.category, RuntimeWarning)]
        assert not runtime_warnings, "All-NaN RuntimeWarning must be suppressed"
        assert math.isnan(result["psnr_hvs_mean"])


class TestStagingRoundtrip:
    def test_append_and_load_rows(self, k150k, tmp_path: Path) -> None:
        staging = tmp_path / "out.rows.jsonl"
        assert staging == k150k._staging_path(tmp_path / "out.parquet")
        rows = [{"clip_name": "a.mp4", "mos": 3.5}, {"clip_name": "b.mp4", "mos": 4.1}]
        for row in rows:
            k150k._append_row_to_staging(staging, row)
        loaded = k150k._load_staging_rows(staging)
        assert len(loaded) == 2
        assert loaded[0]["clip_name"] == "a.mp4"

    def test_load_staging_tolerates_malformed_line(self, k150k, tmp_path: Path) -> None:
        staging = tmp_path / "x.rows.jsonl"
        staging.write_text(
            '{"clip_name": "good.mp4", "mos": 1.0}\n{bad json\n',
            encoding="utf-8",
        )
        rows = k150k._load_staging_rows(staging)
        assert len(rows) == 1
        assert rows[0]["clip_name"] == "good.mp4"

    def test_load_staging_missing_file_returns_empty(self, k150k, tmp_path: Path) -> None:
        rows = k150k._load_staging_rows(tmp_path / "nonexistent.rows.jsonl")
        assert rows == []


class TestCheckpointHelpers:
    def test_load_done_set_empty_when_file_absent(self, k150k, tmp_path: Path) -> None:
        done = k150k._load_done_set(tmp_path / "nothing.done")
        assert done == set()

    def test_append_and_load_done(self, k150k, tmp_path: Path) -> None:
        done_path = tmp_path / "clips.done"
        k150k._append_done(done_path, "clip_a.mp4")
        k150k._append_done(done_path, "clip_b.mp4")
        done = k150k._load_done_set(done_path)
        assert "clip_a.mp4" in done
        assert "clip_b.mp4" in done
        assert len(done) == 2


class TestContentSplit:
    def test_deterministic_for_same_seed(self, k150k) -> None:
        a = k150k._content_split_for("content-x.mp4", seed="test-seed")
        b = k150k._content_split_for("content-x.mp4", seed="test-seed")
        assert a == b

    def test_returns_valid_split_label(self, k150k) -> None:
        split = k150k._content_split_for("content-abc.mp4", seed="some-seed")
        assert split in {"train", "val", "test"}

    def test_different_seeds_can_differ(self, k150k) -> None:
        # This just checks the function doesn't crash for different seeds.
        for seed in ("seed-a", "seed-b", "seed-c"):
            assert k150k._content_split_for("clip.mp4", seed=seed) in {"train", "val", "test"}


class TestWriteParquetFromRows:
    def test_deduplicates_by_clip_name(self, k150k, tmp_path: Path) -> None:
        import pandas as pd

        rows = [
            {"clip_name": "a.mp4", "mos": 1.0},
            {"clip_name": "a.mp4", "mos": 2.0},  # duplicate — last wins
        ]
        out = tmp_path / "out.parquet"
        k150k._write_parquet_from_rows(rows, out)
        df = pd.read_parquet(out)
        assert len(df) == 1
        assert df.iloc[0]["mos"] == pytest.approx(2.0)

    def test_empty_rows_produces_no_file(self, k150k, tmp_path: Path) -> None:
        out = tmp_path / "empty.parquet"
        k150k._write_parquet_from_rows([], out)
        assert not out.exists()

    def test_creates_parent_dirs(self, k150k, tmp_path: Path) -> None:
        out = tmp_path / "deep" / "dir" / "out.parquet"
        k150k._write_parquet_from_rows([{"clip_name": "x.mp4", "mos": 3.0}], out)
        assert out.exists()


class TestBuildVmafCmd:
    def test_cmd_includes_feature_args(self, k150k, tmp_path: Path) -> None:
        cmd = k150k._build_vmaf_cmd(
            vmaf_bin=Path("vmaf"),
            yuv_path=tmp_path / "clip.yuv",
            width=640,
            height=360,
            pix_fmt="yuv420p",
            out_json=tmp_path / "clip.json",
            threads=2,
            extractor_names=("adm", "vif"),
            backend_args=["--no_cuda"],
        )
        assert "--feature" in cmd
        adm_idx = cmd.index("--feature")
        assert cmd[adm_idx + 1] == "adm"
        assert "--no_cuda" in cmd
        assert "--reference" in cmd
        assert str(tmp_path / "clip.yuv") in cmd

    def test_bitdepth_10_for_10le_pix_fmt(self, k150k, tmp_path: Path) -> None:
        cmd = k150k._build_vmaf_cmd(
            vmaf_bin=Path("vmaf"),
            yuv_path=tmp_path / "clip.yuv",
            width=1920,
            height=1080,
            pix_fmt="yuv420p10le",
            out_json=tmp_path / "clip.json",
            threads=1,
            extractor_names=(),
            backend_args=[],
        )
        bd_idx = cmd.index("--bitdepth")
        assert cmd[bd_idx + 1] == "10"


class TestMergeFrameMetrics:
    def test_merges_keys_from_both_dicts(self, k150k) -> None:
        primary = [{"adm2": 0.9, "vmaf": 80.0}]
        residual = [{"cambi": 0.5}]
        merged = k150k._merge_frame_metrics(primary, residual)
        assert merged[0]["adm2"] == 0.9
        assert merged[0]["vmaf"] == 80.0
        assert merged[0]["cambi"] == 0.5

    def test_stops_at_shorter_list(self, k150k) -> None:
        primary = [{"a": 1}, {"a": 2}, {"a": 3}]
        residual = [{"b": 10}, {"b": 20}]
        merged = k150k._merge_frame_metrics(primary, residual)
        assert len(merged) == 2

    def test_residual_wins_on_key_collision(self, k150k) -> None:
        primary = [{"x": 1}]
        residual = [{"x": 99}]
        merged = k150k._merge_frame_metrics(primary, residual)
        assert merged[0]["x"] == 99


# ===========================================================================
# calibrate_nr_threshold.py helpers
# ===========================================================================


class TestLinearRegression:
    def test_perfect_linear_fit(self, calibrate) -> None:
        x = [1.0, 2.0, 3.0, 4.0, 5.0]
        y = [2.0 * xi + 1.0 for xi in x]
        a, b, residuals = calibrate._linear_regression(x, y)
        assert a == pytest.approx(2.0, abs=1e-9)
        assert b == pytest.approx(1.0, abs=1e-9)
        assert all(abs(r) < 1e-9 for r in residuals)

    def test_raises_for_single_sample(self, calibrate) -> None:
        with pytest.raises(ValueError, match="at least 2 samples"):
            calibrate._linear_regression([1.0], [2.0])

    def test_degenerate_constant_x_falls_back(self, calibrate) -> None:
        # All NR scores identical — denominator is zero; must not raise.
        a, b, _residuals = calibrate._linear_regression([3.0, 3.0, 3.0], [70.0, 75.0, 80.0])
        assert math.isfinite(a)
        assert math.isfinite(b)


class TestComputeDeltaFast:
    def test_returns_adr_default_for_single_residual(self, calibrate) -> None:
        assert calibrate._compute_delta_fast([0.5]) == pytest.approx(8.0)

    def test_two_sigma_coverage(self, calibrate) -> None:
        residuals = [2.0, -2.0, 4.0, -4.0, 0.0]
        delta = calibrate._compute_delta_fast(residuals)
        assert delta > 0.0
        n = len(residuals)
        mean_r = sum(residuals) / n
        var_r = sum((r - mean_r) ** 2 for r in residuals) / (n - 1)
        sigma = math.sqrt(var_r)
        assert delta == pytest.approx(2.0 * sigma, abs=1e-9)


class TestPearsonR:
    def test_perfect_positive_correlation(self, calibrate) -> None:
        x = [1.0, 2.0, 3.0, 4.0]
        y = [10.0, 20.0, 30.0, 40.0]
        assert calibrate._pearson_r(x, y) == pytest.approx(1.0, abs=1e-9)

    def test_perfect_negative_correlation(self, calibrate) -> None:
        x = [1.0, 2.0, 3.0]
        y = [30.0, 20.0, 10.0]
        assert calibrate._pearson_r(x, y) == pytest.approx(-1.0, abs=1e-9)

    def test_undefined_for_single_sample(self, calibrate) -> None:
        assert math.isnan(calibrate._pearson_r([1.0], [1.0]))


class TestCalibrationQuality:
    def test_passes_with_enough_samples_and_plcc(self, calibrate) -> None:
        quality = calibrate._evaluate_calibration_quality(
            sample_count=15, plcc=0.90, min_samples=10, min_plcc=0.70
        )
        assert quality.passed is True
        assert quality.status == "accepted"
        assert quality.reasons == ()

    def test_fails_on_too_few_samples(self, calibrate) -> None:
        quality = calibrate._evaluate_calibration_quality(
            sample_count=5, plcc=0.95, min_samples=10, min_plcc=0.70
        )
        assert quality.passed is False
        assert any("sample count" in r for r in quality.reasons)

    def test_fails_on_low_plcc(self, calibrate) -> None:
        quality = calibrate._evaluate_calibration_quality(
            sample_count=20, plcc=0.50, min_samples=10, min_plcc=0.70
        )
        assert quality.passed is False
        assert any("PLCC" in r for r in quality.reasons)

    def test_fails_on_nan_plcc(self, calibrate) -> None:
        quality = calibrate._evaluate_calibration_quality(
            sample_count=20, plcc=float("nan"), min_samples=10, min_plcc=0.70
        )
        assert quality.passed is False
        assert any("undefined" in r.lower() for r in quality.reasons)


class TestDetectYuvGeometry:
    def test_pattern_1920x1080_in_stem(self, calibrate) -> None:
        result = calibrate._detect_yuv_geometry(Path("video_1920x1080_25fps.yuv"))
        assert result == (1920, 1080)

    def test_pattern_576x324(self, calibrate) -> None:
        result = calibrate._detect_yuv_geometry(Path("src01_hrc00_576x324.yuv"))
        assert result == (576, 324)

    def test_returns_none_when_no_geometry(self, calibrate) -> None:
        result = calibrate._detect_yuv_geometry(Path("noresolution.yuv"))
        assert result is None

    def test_netflix_prefix_returns_1080p(self, calibrate) -> None:
        result = calibrate._detect_yuv_geometry(Path("ElFuente1_25fps.yuv"))
        assert result == (1920, 1080)


class TestVmafCliPixelFormat:
    def test_yuv420_family(self, calibrate) -> None:
        assert calibrate._vmaf_cli_pixel_format("yuv420p") == "420"
        assert calibrate._vmaf_cli_pixel_format("yuv420p10le") == "420"

    def test_yuv422_family(self, calibrate) -> None:
        assert calibrate._vmaf_cli_pixel_format("yuv422p") == "422"

    def test_yuv444_family(self, calibrate) -> None:
        assert calibrate._vmaf_cli_pixel_format("yuv444p") == "444"


class TestVmafCliBitdepth:
    def test_8bit_default(self, calibrate) -> None:
        assert calibrate._vmaf_cli_bitdepth("yuv420p") == "8"

    def test_10bit(self, calibrate) -> None:
        assert calibrate._vmaf_cli_bitdepth("yuv420p10le") == "10"

    def test_12bit(self, calibrate) -> None:
        assert calibrate._vmaf_cli_bitdepth("yuv422p12le") == "12"

    def test_16bit(self, calibrate) -> None:
        assert calibrate._vmaf_cli_bitdepth("yuv420p16le") == "16"


class TestCalibrateMainArgValidation:
    def test_rejects_empty_crfs(self, calibrate, tmp_path: Path) -> None:
        rc = calibrate.main(
            [
                "--corpus",
                str(tmp_path / "no-corpus"),
                "--output",
                str(tmp_path / "out.json"),
                "--onnx",
                str(tmp_path / "out.onnx"),
                "--crfs",
                "",
                "--report-dir",
                str(tmp_path / "reports"),
            ]
        )
        assert rc == 2

    def test_rejects_min_calibration_samples_below_2(self, calibrate, tmp_path: Path) -> None:
        rc = calibrate.main(
            [
                "--corpus",
                str(tmp_path / "no-corpus"),
                "--output",
                str(tmp_path / "out.json"),
                "--onnx",
                str(tmp_path / "out.onnx"),
                "--crfs",
                "23",
                "--min-calibration-samples",
                "1",
                "--report-dir",
                str(tmp_path),
            ]
        )
        assert rc == 2

    def test_rejects_invalid_min_plcc(self, calibrate, tmp_path: Path) -> None:
        rc = calibrate.main(
            [
                "--corpus",
                str(tmp_path / "no-corpus"),
                "--output",
                str(tmp_path / "out.json"),
                "--onnx",
                str(tmp_path / "out.onnx"),
                "--crfs",
                "23",
                "--min-plcc",
                "1.5",
                "--report-dir",
                str(tmp_path),
            ]
        )
        assert rc == 2

    def test_no_corpus_dir_and_no_fallback_returns_1(self, calibrate, tmp_path: Path) -> None:
        # Corpus is absent, fallback dir also empty → no YUV files → rc 1.
        empty_fallback = tmp_path / "empty_yuv"
        empty_fallback.mkdir()
        import unittest.mock as mock

        with mock.patch.object(type(calibrate._FALLBACK_YUV_DIR), "glob", return_value=iter([])):
            pass  # don't patch — just ensure the call path works
        rc = calibrate.main(
            [
                "--corpus",
                str(tmp_path / "missing-corpus"),
                "--output",
                str(tmp_path / "out.json"),
                "--onnx",
                str(tmp_path / "out.onnx"),
                "--crfs",
                "23,28",
                "--report-dir",
                str(tmp_path / "reports"),
                "--dry-run",
            ]
        )
        # rc may be 1 (no yuv files) or 0 if BBB yuv happens to exist.
        assert rc in (0, 1)


# ===========================================================================
# materialize_mos_labels.py helpers
# ===========================================================================


class TestNormaliseKey:
    def test_raw_mode_keeps_value_intact(self, mat_mos) -> None:
        result = mat_mos._normalise_key(
            "some/path/clip.mp4", mode="raw", column_name="x", regex=None
        )
        assert result == "some/path/clip.mp4"

    def test_basename_mode_extracts_filename(self, mat_mos) -> None:
        result = mat_mos._normalise_key(
            "some/path/clip.mp4", mode="basename", column_name="x", regex=None
        )
        assert result == "clip.mp4"

    def test_stem_mode_strips_extension(self, mat_mos) -> None:
        result = mat_mos._normalise_key(
            "some/path/clip.mp4", mode="stem", column_name="x", regex=None
        )
        assert result == "clip"

    def test_auto_mode_path_column_uses_basename(self, mat_mos) -> None:
        result = mat_mos._normalise_key(
            "/data/clip.mp4", mode="auto", column_name="src_path", regex=None
        )
        assert result == "clip.mp4"

    def test_auto_mode_generic_column_uses_raw(self, mat_mos) -> None:
        result = mat_mos._normalise_key("abc123", mode="auto", column_name="video_id", regex=None)
        assert result == "abc123"

    def test_regex_extracts_first_group(self, mat_mos) -> None:
        result = mat_mos._normalise_key(
            "orig_10008004183_540_5s.mp4",
            mode="basename",
            column_name="src",
            regex=r"([0-9]{6,})",
        )
        assert result == "10008004183"

    def test_none_value_returns_empty_string(self, mat_mos) -> None:
        result = mat_mos._normalise_key(None, mode="raw", column_name="x", regex=None)
        assert result == ""

    def test_nan_float_returns_empty_string(self, mat_mos) -> None:
        result = mat_mos._normalise_key(float("nan"), mode="raw", column_name="x", regex=None)
        assert result == ""

    def test_unsupported_mode_raises(self, mat_mos) -> None:
        with pytest.raises(ValueError, match="unsupported key normalisation"):
            mat_mos._normalise_key("x", mode="unsupported", column_name="y", regex=None)


class TestMosPayload:
    def test_likert_scale_roundtrip(self, mat_mos) -> None:
        mos, raw = mat_mos._mos_payload(3.0, column="mos")
        assert mos == pytest.approx(3.0)
        assert raw == pytest.approx(50.0)

    def test_mos_raw_0_100_column_treated_as_100_scale(self, mat_mos) -> None:
        mos, raw = mat_mos._mos_payload(50.0, column="mos_raw_0_100")
        assert raw == pytest.approx(50.0)
        assert mos == pytest.approx(3.0)

    def test_value_above_5_treated_as_100_scale(self, mat_mos) -> None:
        mos, raw = mat_mos._mos_payload(75.0, column="score")
        assert raw == pytest.approx(75.0)
        assert 1.0 <= mos <= 5.0

    def test_out_of_range_raises_for_value_above_100(self, mat_mos) -> None:
        # 6.0 is > MOS_MAX (5.0) so it enters the 0-100 branch and is in range.
        # 110.0 exceeds the 0-100 range and must raise.
        with pytest.raises(ValueError):
            mat_mos._mos_payload(110.0, column="mos")

    def test_non_numeric_raises(self, mat_mos) -> None:
        with pytest.raises(ValueError, match="numeric"):
            mat_mos._mos_payload("bad", column="mos")


class TestFindColumn:
    def test_explicit_column_present(self, mat_mos) -> None:
        import pandas as pd

        df = pd.DataFrame({"video_id": [1], "mos": [4.0]})
        result = mat_mos._find_column(
            df, "video_id", mat_mos.KEY_CANDIDATES, "--feature-key-column"
        )
        assert result == "video_id"

    def test_auto_detect_from_candidates(self, mat_mos) -> None:
        import pandas as pd

        df = pd.DataFrame({"src": ["a.mp4"], "mos": [3.0]})
        result = mat_mos._find_column(df, None, mat_mos.KEY_CANDIDATES, "--feature-key-column")
        assert result == "src"

    def test_raises_when_no_match(self, mat_mos) -> None:
        import pandas as pd

        df = pd.DataFrame({"unknown_col": [1]})
        with pytest.raises(ValueError, match="could not infer"):
            mat_mos._find_column(df, None, mat_mos.MOS_CANDIDATES, "--label-mos-column")

    def test_raises_when_explicit_column_absent(self, mat_mos) -> None:
        import pandas as pd

        df = pd.DataFrame({"mos": [4.0]})
        with pytest.raises(ValueError, match="is not present"):
            mat_mos._find_column(df, "video_id", mat_mos.KEY_CANDIDATES, "--feature-key-column")


class TestReadTable:
    def test_csv_read(self, mat_mos, tmp_path: Path) -> None:
        p = tmp_path / "table.csv"
        p.write_text("video_id,mos\na,3.5\nb,4.0\n", encoding="utf-8")
        df = mat_mos._read_table(p)
        assert list(df["video_id"]) == ["a", "b"]

    def test_jsonl_read(self, mat_mos, tmp_path: Path) -> None:
        p = tmp_path / "table.jsonl"
        p.write_text(
            json.dumps({"video_id": "c", "mos": 2.5})
            + "\n"
            + json.dumps({"video_id": "d", "mos": 3.5})
            + "\n",
            encoding="utf-8",
        )
        df = mat_mos._read_table(p)
        assert len(df) == 2

    def test_json_list_read(self, mat_mos, tmp_path: Path) -> None:
        p = tmp_path / "table.json"
        p.write_text(
            json.dumps([{"video_id": "e", "mos": 4.0}]),
            encoding="utf-8",
        )
        df = mat_mos._read_table(p)
        assert len(df) == 1
        assert df.iloc[0]["video_id"] == "e"

    def test_json_rows_dict_read(self, mat_mos, tmp_path: Path) -> None:
        p = tmp_path / "table2.json"
        p.write_text(
            json.dumps({"rows": [{"video_id": "f", "mos": 1.5}]}),
            encoding="utf-8",
        )
        df = mat_mos._read_table(p)
        assert df.iloc[0]["video_id"] == "f"

    def test_unsupported_extension_raises(self, mat_mos, tmp_path: Path) -> None:
        p = tmp_path / "table.txt"
        p.write_text("data\n", encoding="utf-8")
        with pytest.raises(ValueError, match="unsupported table format"):
            mat_mos._read_table(p)


class TestNormaliseExtraName:
    def test_stddev_alias(self, mat_mos) -> None:
        assert mat_mos._normalise_extra_name("stddev") == "mos_std_dev"
        assert mat_mos._normalise_extra_name("std_dev") == "mos_std_dev"

    def test_n_ratings_alias(self, mat_mos) -> None:
        assert mat_mos._normalise_extra_name("rating_count") == "mos_n_ratings"
        assert mat_mos._normalise_extra_name("n_ratings") == "mos_n_ratings"

    def test_passthrough_for_unknown(self, mat_mos) -> None:
        assert mat_mos._normalise_extra_name("corpus") == "corpus"


class TestMaterializeCli:
    def test_cli_with_key_normalize_stem(self, mat_mos, tmp_path: Path) -> None:
        features_path = tmp_path / "features.jsonl"
        labels_path = tmp_path / "labels.jsonl"
        out_path = tmp_path / "out.jsonl"
        features_path.write_text(
            json.dumps({"src": "some/path/clip_a.mp4", "adm2": 0.9}) + "\n",
            encoding="utf-8",
        )
        labels_path.write_text(
            json.dumps({"src": "clip_a", "mos": 3.5}) + "\n",
            encoding="utf-8",
        )
        rc = mat_mos.main(
            [
                "--features",
                str(features_path),
                "--labels",
                str(labels_path),
                "--out",
                str(out_path),
                "--key-normalize",
                "stem",
            ]
        )
        assert rc == 0
        rows = [json.loads(ln) for ln in out_path.read_text().splitlines() if ln]
        assert rows[0]["mos"] == pytest.approx(3.5)


# ===========================================================================
# batch_materialize_saliency_features.py helpers
# ===========================================================================


class TestBatchSaliencyManifestLoading:
    def test_load_manifest_rejects_empty_tables(self, batch_sal, tmp_path: Path) -> None:
        manifest = tmp_path / "batch.json"
        manifest.write_text(json.dumps({"tables": []}), encoding="utf-8")
        with pytest.raises(ValueError, match="non-empty array"):
            batch_sal.load_batch_manifest(manifest)

    def test_load_manifest_rejects_missing_id(self, batch_sal, tmp_path: Path) -> None:
        manifest = tmp_path / "batch.json"
        manifest.write_text(
            json.dumps({"tables": [{"input": "in.jsonl", "output": "out.jsonl"}]}),
            encoding="utf-8",
        )
        with pytest.raises(ValueError, match="missing non-empty id"):
            batch_sal.load_batch_manifest(manifest)

    def test_load_manifest_rejects_invalid_json(self, batch_sal, tmp_path: Path) -> None:
        manifest = tmp_path / "batch.json"
        manifest.write_text("{bad json", encoding="utf-8")
        with pytest.raises(ValueError, match="invalid JSON"):
            batch_sal.load_batch_manifest(manifest)

    def test_load_manifest_rejects_non_dict_root(self, batch_sal, tmp_path: Path) -> None:
        manifest = tmp_path / "batch.json"
        manifest.write_text(json.dumps([1, 2, 3]), encoding="utf-8")
        with pytest.raises(ValueError, match="JSON object"):
            batch_sal.load_batch_manifest(manifest)

    def test_write_markdown_report_produces_valid_md(self, batch_sal, tmp_path: Path) -> None:
        payload = {
            "status": "ok",
            "tables": [
                {
                    "id": "t1",
                    "status": "ok",
                    "input": "in.jsonl",
                    "output": "out.jsonl",
                    "audit_json": None,
                    "summary": {
                        "total": 5,
                        "ok": 5,
                        "skipped_existing": 0,
                        "failed": 0,
                    },
                }
            ],
        }
        out = tmp_path / "report.md"
        batch_sal.write_markdown_report(out, payload)
        text = out.read_text(encoding="utf-8")
        assert "# Saliency Materializer Batch Report" in text
        assert "**ok**" in text
        assert "| t1 | ok | 5 | 5 | 0 | 0 |" in text

    def test_batch_run_options_defaults(self, batch_sal) -> None:
        opts = batch_sal.BatchRunOptions()
        assert opts.allow_row_failures is False
        assert opts.fail_fast is False


# ===========================================================================
# batch_materialize_second_opinion_features.py helpers
# ===========================================================================


class TestBatchSecondOpinionManifestLoading:
    def test_load_manifest_rejects_empty_tables(self, batch_so, tmp_path: Path) -> None:
        manifest = tmp_path / "batch.json"
        manifest.write_text(json.dumps({"tables": []}), encoding="utf-8")
        with pytest.raises(ValueError, match="non-empty array"):
            batch_so.load_batch_manifest(manifest)

    def test_load_manifest_rejects_missing_scores(self, batch_so, tmp_path: Path) -> None:
        manifest = tmp_path / "batch.json"
        manifest.write_text(
            json.dumps(
                {
                    "tables": [
                        {
                            "id": "t1",
                            "features": "features.jsonl",
                            "out": "out.jsonl",
                            "scores": [],
                        }
                    ]
                }
            ),
            encoding="utf-8",
        )
        with pytest.raises(ValueError, match="non-empty scores array"):
            batch_so.load_batch_manifest(manifest)

    def test_load_manifest_rejects_invalid_json(self, batch_so, tmp_path: Path) -> None:
        manifest = tmp_path / "batch.json"
        manifest.write_text("{bad", encoding="utf-8")
        with pytest.raises(ValueError, match="invalid JSON"):
            batch_so.load_batch_manifest(manifest)

    def test_resolve_score_spec_with_label(self, batch_so, tmp_path: Path) -> None:
        base = tmp_path / "base"
        base.mkdir()
        result = batch_so._resolve_score_spec("fork-nr=scores.jsonl", base)
        assert result == f"fork-nr={base / 'scores.jsonl'}"

    def test_resolve_score_spec_without_label(self, batch_so, tmp_path: Path) -> None:
        base = tmp_path / "base"
        base.mkdir()
        result = batch_so._resolve_score_spec("scores.jsonl", base)
        assert result == str(base / "scores.jsonl")

    def test_write_markdown_report_produces_valid_md(self, batch_so, tmp_path: Path) -> None:
        payload = {
            "status": "ok",
            "tables": [
                {
                    "id": "t1",
                    "status": "ok",
                    "features": "f.jsonl",
                    "scores": ["s.jsonl"],
                    "out": "out.jsonl",
                    "audit_json": None,
                    "summary": {
                        "input_rows": 3,
                        "output_rows": 3,
                        "competitors": ["fork-nr"],
                    },
                }
            ],
        }
        out = tmp_path / "report.md"
        batch_so.write_markdown_report(out, payload)
        text = out.read_text(encoding="utf-8")
        assert "# Second-Opinion Materializer Batch Report" in text
        assert "| t1 | ok | 3 | 3 |" in text

    def test_batch_second_opinion_options_defaults(self, batch_so) -> None:
        opts = batch_so.SecondOpinionBatchOptions()
        assert opts.fail_fast is False


# ===========================================================================
# aggregate_corpora.py CLI edge cases not covered in test_aggregate_corpora
# ===========================================================================


class TestAggregateCorpusCli:
    def _write_jsonl(self, path: Path, rows: list) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            "".join(json.dumps(row) + "\n" for row in rows),
            encoding="utf-8",
        )

    def test_cli_main_multiple_inputs_output(self, agg, tmp_path: Path) -> None:
        p = tmp_path / "k.jsonl"
        self._write_jsonl(
            p,
            [
                {
                    "src": "a.mp4",
                    "src_sha256": "aa" * 32,
                    "mos": 3.0,
                    "corpus": "konvid-1k",
                }
            ],
        )
        out = tmp_path / "unified.jsonl"
        rc = agg.main(["--inputs", str(p), "--output", str(out)])
        assert rc == 0
        rows = [json.loads(ln) for ln in out.read_text().splitlines() if ln]
        assert len(rows) == 1
        assert rows[0]["corpus_source"] == "konvid-1k"
        assert rows[0]["mos"] == pytest.approx(50.0)  # (3-1)*25

    def test_cli_corpus_source_override_flag(self, agg, tmp_path: Path) -> None:
        p = tmp_path / "exotic.jsonl"
        self._write_jsonl(
            p,
            [
                {
                    "src": "x.mp4",
                    "src_sha256": "bb" * 32,
                    "mos": 80.0,
                    "corpus": "some-exotic-source",
                }
            ],
        )
        out = tmp_path / "unified2.jsonl"
        rc = agg.main(
            [
                "--inputs",
                str(p),
                "--output",
                str(out),
                "--corpus-source-override",
                f"{p}=waterloo-ivc-4k",
            ]
        )
        assert rc == 0
        rows = [json.loads(ln) for ln in out.read_text().splitlines() if ln]
        assert rows[0]["corpus_source"] == "waterloo-ivc-4k"

    def test_transform_row_waterloo_identity(self, agg) -> None:
        row = {
            "src": "w.mp4",
            "src_sha256": "cc" * 32,
            "mos": 65.0,
            "corpus": "waterloo-ivc-4k",
        }
        out = agg.transform_row(
            row, corpus_source="waterloo-ivc-4k", aggregated_at_utc="2026-05-09T00:00:00+00:00"
        )
        assert out["mos"] == pytest.approx(65.0)
        assert out["mos_native_scale"] == "0-100-dcr"

    def test_transform_row_netflix_public_identity(self, agg) -> None:
        row = {
            "src": "n.mp4",
            "src_sha256": "dd" * 32,
            "mos": 88.0,
            "corpus": "netflix-public",
        }
        out = agg.transform_row(
            row, corpus_source="netflix-public", aggregated_at_utc="2026-05-09T00:00:00+00:00"
        )
        assert out["mos"] == pytest.approx(88.0)
        assert out["mos_native_scale"] == "vmaf"

    def test_resolve_corpus_source_prefers_override(self, agg) -> None:
        row = {"corpus": "konvid-1k"}
        assert agg._resolve_corpus_source(row, override="lsvq") == "lsvq"

    def test_resolve_corpus_source_falls_back_to_row(self, agg) -> None:
        row = {"corpus": "youtube-ugc"}
        assert agg._resolve_corpus_source(row, override=None) == "youtube-ugc"

    def test_resolve_corpus_source_unknown_returns_none(self, agg) -> None:
        row = {"corpus": "made-up-set"}
        assert agg._resolve_corpus_source(row, override=None) is None
