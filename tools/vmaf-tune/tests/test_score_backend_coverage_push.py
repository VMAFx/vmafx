# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Coverage push for vmaftune.score_backend — gaps identified by inspection.

Covers branches not exercised by the existing test_score_backend.py
(backend detection reads ``vmaf --list-backends`` since ADR-1874; its
tests live there):

* :class:`NRProxyBackend` — ``calibrated_vmaf_score`` clipping,
  ``is_far_from_target``, ``nr_implied_direction``,
  ``_resolve_sidecar_float`` from JSON, default-field resolution,
  ``clear_cache``, ``_luma_plane_bytes`` + ``_chroma_plane_bytes`` +
  ``_frame_bytes`` helpers for every pix_fmt.
* :func:`_load_ort_session` error paths — missing model file.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.score_backend import (
    NR_PROXY_DEFAULT_CALIBRATION_INTERCEPT,
    NR_PROXY_DEFAULT_CALIBRATION_SLOPE,
    NR_PROXY_DEFAULT_DELTA_FAST,
    NRProxyBackend,
    NRProxyBackendError,
    _chroma_plane_bytes,
    _frame_bytes,
    _luma_plane_bytes,
)

# ---------------------------------------------------------------------------
# Pixel-format byte helpers
# ---------------------------------------------------------------------------


class TestPixFmtHelpers:
    def test_luma_yuv420p_8bit(self) -> None:
        assert _luma_plane_bytes(320, 240, "yuv420p") == 320 * 240

    def test_luma_yuv420p10le(self) -> None:
        # 10-bit LE: 2 bytes per luma sample
        assert _luma_plane_bytes(320, 240, "yuv420p10le") == 320 * 240 * 2

    def test_luma_yuv420p12le(self) -> None:
        assert _luma_plane_bytes(320, 240, "yuv420p12le") == 320 * 240 * 2

    def test_luma_yuv420p16le(self) -> None:
        assert _luma_plane_bytes(320, 240, "yuv420p16le") == 320 * 240 * 2

    def test_chroma_yuv420p(self) -> None:
        # yuv420: chroma planes are (w//2)*(h//2)*2
        assert _chroma_plane_bytes(320, 240, "yuv420p") == (320 // 2) * (240 // 2) * 2

    def test_chroma_yuv422p(self) -> None:
        # yuv422: (w//2)*h*2
        assert _chroma_plane_bytes(320, 240, "yuv422p") == (320 // 2) * 240 * 2

    def test_chroma_yuv444p(self) -> None:
        # yuv444: w*h*2
        assert _chroma_plane_bytes(320, 240, "yuv444p") == 320 * 240 * 2

    def test_frame_bytes_is_luma_plus_chroma(self) -> None:
        w, h = 320, 240
        fmt = "yuv420p"
        assert _frame_bytes(w, h, fmt) == _luma_plane_bytes(w, h, fmt) + _chroma_plane_bytes(
            w, h, fmt
        )

    def test_frame_bytes_10bit(self) -> None:
        w, h = 1920, 1080
        fmt = "yuv420p10le"
        assert _frame_bytes(w, h, fmt) == _luma_plane_bytes(w, h, fmt) + _chroma_plane_bytes(
            w, h, fmt
        )


# ---------------------------------------------------------------------------
# NRProxyBackend — pure-logic tests (no onnxruntime, no real model)
# ---------------------------------------------------------------------------


class TestNRProxyBackendCalibration:
    def _backend_with_delta(self, delta: float) -> NRProxyBackend:
        return NRProxyBackend(
            model_path=Path("/nonexistent/model.onnx"),
            delta_fast=delta,
            use_gpu_ep=False,
        )

    def test_calibrated_vmaf_score_default_slope(self) -> None:
        backend = self._backend_with_delta(8.0)
        # Default slope=20, intercept=0 → score = 20*1.0 + 0 = 20, clamped to [0,100]
        result = backend.calibrated_vmaf_score(1.0)
        assert result == pytest.approx(20.0)

    def test_calibrated_vmaf_score_clamps_below_zero(self) -> None:
        backend = self._backend_with_delta(8.0)
        # Very negative raw score must clamp to 0
        result = backend.calibrated_vmaf_score(-999.0)
        assert result == pytest.approx(0.0)

    def test_calibrated_vmaf_score_clamps_above_100(self) -> None:
        backend = self._backend_with_delta(8.0)
        # Very large raw score must clamp to 100
        result = backend.calibrated_vmaf_score(1e9)
        assert result == pytest.approx(100.0)

    def test_is_far_from_target_true(self) -> None:
        backend = self._backend_with_delta(2.0)
        # calibrated(1.0) = 20.0; target=90.0; gap=70 > delta=2 → far
        assert backend.is_far_from_target(1.0, 90.0) is True

    def test_is_far_from_target_false(self) -> None:
        # With slope=1, intercept=0 (via sidecar override): calibrated(90) = 90; target=90; gap=0
        backend = NRProxyBackend(
            model_path=Path("/nonexistent/model.onnx"),
            delta_fast=5.0,
            use_gpu_ep=False,
        )
        # Override slopes via sidecar-less path: calibrated_vmaf_score uses defaults
        # calibrated(4.5) = 20*4.5+0 = 90; target=90; gap=0 < 5
        assert backend.is_far_from_target(4.5, 90.0) is False

    def test_nr_implied_direction_tighter_when_above_target(self) -> None:
        backend = self._backend_with_delta(8.0)
        # calibrated(4.5) = 90 >= target 85 → tighter
        assert backend.nr_implied_direction(4.5, 85.0) == "tighter"

    def test_nr_implied_direction_looser_when_below_target(self) -> None:
        backend = self._backend_with_delta(8.0)
        # calibrated(0.01) ≈ 0.2 < target 90 → looser
        assert backend.nr_implied_direction(0.01, 90.0) == "looser"

    def test_delta_fast_overridden_by_explicit_kwarg(self) -> None:
        backend = NRProxyBackend(
            model_path=Path("/nonexistent/model.onnx"),
            delta_fast=3.14,
            use_gpu_ep=False,
        )
        assert backend.calibration_threshold == pytest.approx(3.14)

    def test_sidecar_provides_delta_fast(self, tmp_path: Path) -> None:
        sidecar = tmp_path / "model.json"
        sidecar.write_text(json.dumps({"calibration_threshold": 6.5}), encoding="utf-8")
        backend = NRProxyBackend(
            model_path=tmp_path / "model.onnx",
            sidecar_path=sidecar,
            use_gpu_ep=False,
        )
        assert backend.calibration_threshold == pytest.approx(6.5)

    def test_sidecar_provides_slope_and_intercept(self, tmp_path: Path) -> None:
        sidecar = tmp_path / "model.json"
        sidecar.write_text(
            json.dumps(
                {
                    "calibration_threshold": 8.0,
                    "calibration_slope": 15.0,
                    "calibration_intercept": 10.0,
                }
            ),
            encoding="utf-8",
        )
        backend = NRProxyBackend(
            model_path=tmp_path / "model.onnx",
            sidecar_path=sidecar,
            use_gpu_ep=False,
        )
        assert backend.calibration_slope == pytest.approx(15.0)
        assert backend.calibration_intercept == pytest.approx(10.0)
        # calibrated(1.0) = 15*1 + 10 = 25
        assert backend.calibrated_vmaf_score(1.0) == pytest.approx(25.0)

    def test_missing_sidecar_uses_defaults(self, tmp_path: Path) -> None:
        backend = NRProxyBackend(
            model_path=tmp_path / "model.onnx",
            sidecar_path=tmp_path / "nonexistent.json",
            use_gpu_ep=False,
        )
        assert backend.calibration_threshold == pytest.approx(NR_PROXY_DEFAULT_DELTA_FAST)
        assert backend.calibration_slope == pytest.approx(NR_PROXY_DEFAULT_CALIBRATION_SLOPE)
        assert backend.calibration_intercept == pytest.approx(
            NR_PROXY_DEFAULT_CALIBRATION_INTERCEPT
        )

    def test_malformed_sidecar_uses_defaults(self, tmp_path: Path) -> None:
        sidecar = tmp_path / "bad.json"
        sidecar.write_text("not valid json", encoding="utf-8")
        backend = NRProxyBackend(
            model_path=tmp_path / "model.onnx",
            sidecar_path=sidecar,
            use_gpu_ep=False,
        )
        assert backend.calibration_threshold == pytest.approx(NR_PROXY_DEFAULT_DELTA_FAST)

    def test_clear_cache_empties_score_cache(self, tmp_path: Path) -> None:
        backend = NRProxyBackend(
            model_path=tmp_path / "model.onnx",
            delta_fast=5.0,
            use_gpu_ep=False,
        )
        # Manually populate the cache
        backend._cache[("dist.yuv", 320, 240)] = 3.5  # type: ignore[index]
        assert len(backend._cache) == 1
        backend.clear_cache()
        assert len(backend._cache) == 0

    def test_sidecar_auto_resolved_alongside_model(self, tmp_path: Path) -> None:
        model = tmp_path / "nr_metric_v1.onnx"
        sidecar = tmp_path / "nr_metric_v1.json"
        sidecar.write_text(json.dumps({"calibration_threshold": 4.0}), encoding="utf-8")
        backend = NRProxyBackend(model_path=model, use_gpu_ep=False)
        # sidecar_path should be auto-resolved to the .json sibling
        assert backend.sidecar_path == sidecar
        assert backend.calibration_threshold == pytest.approx(4.0)


# ---------------------------------------------------------------------------
# _load_ort_session error — missing model file
# ---------------------------------------------------------------------------


class TestLoadOrtSession:
    def test_missing_model_file_raises_nr_proxy_backend_error(self, tmp_path: Path) -> None:
        from vmaftune.score_backend import _load_ort_session

        missing = tmp_path / "ghost.onnx"
        with pytest.raises(NRProxyBackendError, match="not found"):
            _load_ort_session(missing)


# ---------------------------------------------------------------------------
# NRProxyBackend.score_nr via cache hit (no ONNX needed)
# ---------------------------------------------------------------------------


class TestNRProxyBackendScoreNrCacheHit:
    def test_cache_hit_returns_cached_value(self, tmp_path: Path) -> None:
        backend = NRProxyBackend(
            model_path=tmp_path / "model.onnx",
            delta_fast=5.0,
            use_gpu_ep=False,
        )
        # Pre-populate cache
        dist = tmp_path / "dist.yuv"
        key = (str(dist), 320, 240)
        backend._cache[key] = 3.5  # type: ignore[index]

        result = backend.score_nr(dist, width=320, height=240)
        assert result.nr_score == pytest.approx(3.5)
        assert result.from_cache is True
