# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for NRProxyBackend (ADR-0624 / ADR-0615).

All tests are subprocess- and I/O-free: onnxruntime and numpy are
either absent (ImportError path) or replaced with in-process stubs.
The test_bisect_nr_integration tests exercise the bisect-loop sentinel
path with a DeterministicFR + NR stub wired together.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

import pytest

# Make src/ importable without an editable install.
_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.score_backend import (  # noqa: E402
    NR_MODEL_INPUT_HW,
    NR_PROXY_DEFAULT_CALIBRATION_INTERCEPT,
    NR_PROXY_DEFAULT_CALIBRATION_SLOPE,
    NR_PROXY_DEFAULT_DELTA_FAST,
    NRProbeResult,
    NRProxyBackend,
    NRProxyBackendError,
    _chroma_plane_bytes,
    _extract_middle_luma_frame,
    _frame_bytes,
    _luma_plane_bytes,
    _resize_luma_224,
)

# ---------------------------------------------------------------------------
# Helpers / stubs
# ---------------------------------------------------------------------------


def _write_synthetic_yuv(path: Path, *, width: int, height: int, pix_fmt: str, n_frames: int = 3):
    """Write n_frames of synthetic all-gray luma (128) YUV to path."""
    frame_sz = _frame_bytes(width, height, pix_fmt)
    with path.open("wb") as fh:
        for _ in range(n_frames):
            fh.write(bytes([128]) * frame_sz)


class _FakeOrtSession:
    """Minimal stand-in for ``onnxruntime.InferenceSession``."""

    def __init__(self, score: float = 72.0):
        self._score = score
        self._calls: list[dict] = []

    def get_inputs(self):
        return [SimpleNamespace(name="input")]

    def get_providers(self):
        return ["CPUExecutionProvider"]

    def run(self, output_names, input_feed):
        import numpy as np

        self._calls.append(input_feed)
        arr = np.array([[[[self._score]]]], dtype=np.float32)
        return [arr]


class _FakeOrt:
    """Minimal onnxruntime module stand-in."""

    class SessionOptions:
        log_severity_level = 3

    class InferenceSession:
        def __init__(self, model_path: str, sess_options=None, providers=None):
            self._model_path = model_path
            self._providers = providers or []

        def get_inputs(self):
            return [SimpleNamespace(name="input")]

        def get_providers(self):
            return self._providers

        def run(self, output_names, input_feed):
            import numpy as np

            return [np.array([[[[72.0]]]], dtype=np.float32)]

    @staticmethod
    def get_available_providers():
        return ["CPUExecutionProvider"]


# ---------------------------------------------------------------------------
# _luma_plane_bytes / _chroma_plane_bytes / _frame_bytes
# ---------------------------------------------------------------------------


class TestPlaneBytes:
    def test_yuv420p_luma(self):
        assert _luma_plane_bytes(320, 240, "yuv420p") == 320 * 240

    def test_yuv420p10le_luma_is_double(self):
        assert _luma_plane_bytes(320, 240, "yuv420p10le") == 320 * 240 * 2

    def test_yuv420p_chroma(self):
        # Two half-res chroma planes: (W/2) * (H/2) * 2
        assert _chroma_plane_bytes(320, 240, "yuv420p") == (320 // 2) * (240 // 2) * 2

    def test_yuv444p_chroma(self):
        # Two full-res chroma planes
        assert _chroma_plane_bytes(320, 240, "yuv444p") == 320 * 240 * 2

    def test_frame_bytes_yuv420p(self):
        w, h = 320, 240
        expected = w * h + (w // 2) * (h // 2) * 2
        assert _frame_bytes(w, h, "yuv420p") == expected


# ---------------------------------------------------------------------------
# _extract_middle_luma_frame
# ---------------------------------------------------------------------------


class TestExtractMiddleLumaFrame:
    def test_reads_middle_frame_8bit(self, tmp_path):
        import numpy as np

        yuv = tmp_path / "test.yuv"
        _write_synthetic_yuv(yuv, width=16, height=8, pix_fmt="yuv420p", n_frames=5)
        frame = _extract_middle_luma_frame(yuv, width=16, height=8, pix_fmt="yuv420p")
        assert frame.shape == (8, 16)
        assert frame.dtype == np.uint8
        assert int(frame.mean()) == 128

    def test_middle_frame_index_is_n_over_2(self, tmp_path):
        """Frame 2 out of 5 is index 5//2=2; mark it with value 99."""

        w, h = 8, 4
        luma_sz = _luma_plane_bytes(w, h, "yuv420p")
        chroma_sz = _chroma_plane_bytes(w, h, "yuv420p")
        frame_sz = luma_sz + chroma_sz
        # Write 5 frames: frames 0,1,3,4 = 128; frame 2 = 99
        data = bytearray(frame_sz * 5)
        for i in range(5):
            start = i * frame_sz
            val = 99 if i == 2 else 128
            for j in range(luma_sz):
                data[start + j] = val
        yuv = tmp_path / "marked.yuv"
        yuv.write_bytes(bytes(data))

        frame = _extract_middle_luma_frame(yuv, width=w, height=h, pix_fmt="yuv420p")
        assert int(frame.mean()) == 99

    def test_raises_on_missing_file(self, tmp_path):
        missing = tmp_path / "missing.yuv"
        with pytest.raises(NRProxyBackendError, match="cannot stat"):
            _extract_middle_luma_frame(missing, width=16, height=8, pix_fmt="yuv420p")

    def test_raises_on_truncated_file(self, tmp_path):
        yuv = tmp_path / "truncated.yuv"
        yuv.write_bytes(b"\x80" * 10)  # way too short for 16x8
        with pytest.raises(NRProxyBackendError, match="truncated luma read"):
            _extract_middle_luma_frame(yuv, width=16, height=8, pix_fmt="yuv420p")

    def test_10bit_high_byte_extraction(self, tmp_path):
        """yuv420p10le: value 0x0080 (128 << 1) high-byte gives 0."""
        import numpy as np

        w, h = 4, 4
        luma_sz = _luma_plane_bytes(w, h, "yuv420p10le")
        chroma_sz = _chroma_plane_bytes(w, h, "yuv420p10le")
        frame_sz = luma_sz + chroma_sz
        # Each 16-bit LE pixel: 0x0100 → high byte = 0x01 = 1
        data = bytearray(frame_sz)
        for i in range(0, luma_sz, 2):
            data[i] = 0x00  # low byte
            data[i + 1] = 0x01  # high byte → arr_u16 = 0x0100 = 256 >> 8 = 1
        yuv = tmp_path / "10bit.yuv"
        yuv.write_bytes(bytes(data))
        frame = _extract_middle_luma_frame(yuv, width=w, height=h, pix_fmt="yuv420p10le")
        assert frame.dtype == np.uint8
        assert int(frame.mean()) == 1


# ---------------------------------------------------------------------------
# _resize_luma_224
# ---------------------------------------------------------------------------


class TestResizeLuma224:
    def test_no_resize_when_already_224(self):
        import numpy as np

        luma = np.full((224, 224), 128, dtype=np.uint8)
        result = _resize_luma_224(luma, width=224, height=224)
        assert result is luma  # same object returned

    def test_resize_smaller_to_224(self):
        import numpy as np

        luma = np.full((8, 16), 200, dtype=np.uint8)
        result = _resize_luma_224(luma, width=16, height=8)
        assert result.shape == (NR_MODEL_INPUT_HW, NR_MODEL_INPUT_HW)
        assert result.dtype == np.uint8

    def test_resize_preserves_constant_value(self):
        """Constant-value frame should resize to same constant."""
        import numpy as np

        luma = np.full((480, 640), 77, dtype=np.uint8)
        result = _resize_luma_224(luma, width=640, height=480)
        assert result.shape == (224, 224)
        # Allow ±1 for bilinear boundary effects
        assert abs(int(result.mean()) - 77) <= 1


# ---------------------------------------------------------------------------
# NRProxyBackend — delta_fast resolution
# ---------------------------------------------------------------------------


class TestNRProxyBackendDeltaFast:
    def test_default_delta_when_no_sidecar(self, tmp_path):
        model_path = tmp_path / "model.onnx"
        model_path.write_bytes(b"FAKE")
        backend = NRProxyBackend(model_path=model_path)
        # sidecar does not exist → should return NR_PROXY_DEFAULT_DELTA_FAST
        assert backend.calibration_threshold == NR_PROXY_DEFAULT_DELTA_FAST

    def test_reads_calibration_threshold_from_sidecar(self, tmp_path):
        model_path = tmp_path / "model.onnx"
        model_path.write_bytes(b"FAKE")
        sidecar = tmp_path / "model.json"
        sidecar.write_text(json.dumps({"calibration_threshold": 5.5}), encoding="utf-8")
        backend = NRProxyBackend(model_path=model_path, sidecar_path=sidecar)
        assert backend.calibration_threshold == pytest.approx(5.5)

    def test_override_delta_fast_wins_over_sidecar(self, tmp_path):
        model_path = tmp_path / "model.onnx"
        model_path.write_bytes(b"FAKE")
        sidecar = tmp_path / "model.json"
        sidecar.write_text(json.dumps({"calibration_threshold": 5.5}), encoding="utf-8")
        backend = NRProxyBackend(model_path=model_path, sidecar_path=sidecar, delta_fast=3.0)
        assert backend.calibration_threshold == pytest.approx(3.0)

    def test_malformed_sidecar_falls_back_to_default(self, tmp_path):
        model_path = tmp_path / "model.onnx"
        model_path.write_bytes(b"FAKE")
        sidecar = tmp_path / "model.json"
        sidecar.write_text("NOT JSON", encoding="utf-8")
        backend = NRProxyBackend(model_path=model_path, sidecar_path=sidecar)
        assert backend.calibration_threshold == NR_PROXY_DEFAULT_DELTA_FAST

    def test_sidecar_missing_calibration_key_falls_back_to_default(self, tmp_path):
        model_path = tmp_path / "model.onnx"
        model_path.write_bytes(b"FAKE")
        sidecar = tmp_path / "model.json"
        sidecar.write_text(json.dumps({"id": "nr_metric_v1"}), encoding="utf-8")
        backend = NRProxyBackend(model_path=model_path, sidecar_path=sidecar)
        assert backend.calibration_threshold == NR_PROXY_DEFAULT_DELTA_FAST

    def test_sidecar_resolved_to_sibling_json(self, tmp_path):
        """When sidecar_path is None the sibling .json is used automatically."""
        model_path = tmp_path / "nr_metric_v1.onnx"
        model_path.write_bytes(b"FAKE")
        sidecar = tmp_path / "nr_metric_v1.json"
        sidecar.write_text(json.dumps({"calibration_threshold": 7.0}), encoding="utf-8")
        backend = NRProxyBackend(model_path=model_path)
        assert backend.calibration_threshold == pytest.approx(7.0)

    def test_reads_affine_calibration_from_sidecar(self, tmp_path):
        model_path = tmp_path / "model.onnx"
        model_path.write_bytes(b"FAKE")
        sidecar = tmp_path / "model.json"
        sidecar.write_text(
            json.dumps({"calibration_slope": 18.5, "calibration_intercept": 12.0}),
            encoding="utf-8",
        )
        backend = NRProxyBackend(model_path=model_path, sidecar_path=sidecar)
        assert backend.calibration_slope == pytest.approx(18.5)
        assert backend.calibration_intercept == pytest.approx(12.0)
        assert backend.calibrated_vmaf_score(4.0) == pytest.approx(86.0)

    def test_affine_calibration_defaults_to_mos_20x_scale(self, tmp_path):
        model_path = tmp_path / "model.onnx"
        model_path.write_bytes(b"FAKE")
        backend = NRProxyBackend(model_path=model_path, sidecar_path=tmp_path / "missing.json")
        assert backend.calibration_slope == pytest.approx(NR_PROXY_DEFAULT_CALIBRATION_SLOPE)
        assert backend.calibration_intercept == pytest.approx(
            NR_PROXY_DEFAULT_CALIBRATION_INTERCEPT
        )


# ---------------------------------------------------------------------------
# NRProxyBackend — is_far_from_target / nr_implied_direction
# ---------------------------------------------------------------------------


class TestNRProxyBackendDecisionLogic:
    def _make_backend(self, delta: float = 8.0) -> NRProxyBackend:
        backend = NRProxyBackend.__new__(NRProxyBackend)
        object.__setattr__(backend, "model_path", Path("/dev/null"))
        object.__setattr__(backend, "sidecar_path", None)
        object.__setattr__(backend, "use_gpu_ep", False)
        object.__setattr__(backend, "delta_fast", delta)
        object.__setattr__(backend, "_session", None)
        object.__setattr__(backend, "_delta_fast_resolved", delta)
        object.__setattr__(backend, "_calibration_slope_resolved", 1.0)
        object.__setattr__(backend, "_calibration_intercept_resolved", 0.0)
        object.__setattr__(backend, "_cache", {})
        return backend

    def test_far_uses_calibrated_vmaf_scale(self):
        b = self._make_backend(delta=8.0)
        object.__setattr__(b, "_calibration_slope_resolved", 20.0)
        object.__setattr__(b, "_calibration_intercept_resolved", 0.0)
        # raw NR=4.4 maps to VMAF 88, target=90, so it is inside δ=8.
        assert b.is_far_from_target(4.4, 90.0) is False

    def test_far_when_outside_delta(self):
        b = self._make_backend(delta=8.0)
        # |60 - 90| = 30 > 8 → far
        assert b.is_far_from_target(60.0, 90.0) is True

    def test_not_far_when_inside_delta(self):
        b = self._make_backend(delta=8.0)
        # |85 - 90| = 5 < 8 → not far
        assert b.is_far_from_target(85.0, 90.0) is False

    def test_not_far_at_exact_boundary(self):
        b = self._make_backend(delta=8.0)
        # |82 - 90| = 8 == 8 → not far (boundary is inclusive to δ zone)
        assert b.is_far_from_target(82.0, 90.0) is False

    def test_direction_tighter_when_nr_above_target(self):
        b = self._make_backend()
        # NR=95 > target=90 → quality above goal → raise CRF
        assert b.nr_implied_direction(95.0, 90.0) == "tighter"

    def test_direction_looser_when_nr_below_target(self):
        b = self._make_backend()
        # NR=70 < target=90 → quality below goal → lower CRF
        assert b.nr_implied_direction(70.0, 90.0) == "looser"

    def test_direction_tighter_when_nr_equals_target(self):
        # Equal: NR meets target → "tighter" (we can compress more)
        b = self._make_backend()
        assert b.nr_implied_direction(90.0, 90.0) == "tighter"


# ---------------------------------------------------------------------------
# NRProxyBackend — cache behaviour
# ---------------------------------------------------------------------------


class TestNRProxyBackendCache:
    def test_cache_returns_same_score_on_second_call(self, tmp_path):
        """Second call with same path + geometry returns from_cache=True."""

        yuv = tmp_path / "dist.yuv"
        _write_synthetic_yuv(yuv, width=16, height=8, pix_fmt="yuv420p", n_frames=3)

        fake_session = _FakeOrtSession(score=73.5)
        backend = NRProxyBackend.__new__(NRProxyBackend)
        object.__setattr__(backend, "model_path", tmp_path / "fake.onnx")
        object.__setattr__(backend, "sidecar_path", None)
        object.__setattr__(backend, "use_gpu_ep", False)
        object.__setattr__(backend, "delta_fast", 8.0)
        object.__setattr__(backend, "_session", fake_session)
        object.__setattr__(backend, "_delta_fast_resolved", 8.0)
        object.__setattr__(backend, "_cache", {})

        r1 = backend.score_nr(yuv, width=16, height=8)
        r2 = backend.score_nr(yuv, width=16, height=8)
        assert r1.from_cache is False
        assert r2.from_cache is True
        assert r1.nr_score == pytest.approx(r2.nr_score)
        # Session should have been called exactly once
        assert len(fake_session._calls) == 1

    def test_clear_cache_forces_reinference(self, tmp_path):

        yuv = tmp_path / "dist.yuv"
        _write_synthetic_yuv(yuv, width=16, height=8, pix_fmt="yuv420p", n_frames=3)

        fake_session = _FakeOrtSession(score=60.0)
        backend = NRProxyBackend.__new__(NRProxyBackend)
        object.__setattr__(backend, "model_path", tmp_path / "fake.onnx")
        object.__setattr__(backend, "sidecar_path", None)
        object.__setattr__(backend, "use_gpu_ep", False)
        object.__setattr__(backend, "delta_fast", 8.0)
        object.__setattr__(backend, "_session", fake_session)
        object.__setattr__(backend, "_delta_fast_resolved", 8.0)
        object.__setattr__(backend, "_cache", {})

        backend.score_nr(yuv, width=16, height=8)
        assert len(fake_session._calls) == 1
        backend.clear_cache()
        backend.score_nr(yuv, width=16, height=8)
        assert len(fake_session._calls) == 2

    def test_different_geometry_creates_separate_cache_keys(self, tmp_path):

        yuv16x8 = tmp_path / "dist16x8.yuv"
        yuv32x16 = tmp_path / "dist32x16.yuv"
        _write_synthetic_yuv(yuv16x8, width=16, height=8, pix_fmt="yuv420p", n_frames=3)
        _write_synthetic_yuv(yuv32x16, width=32, height=16, pix_fmt="yuv420p", n_frames=3)

        fake_session = _FakeOrtSession(score=80.0)
        backend = NRProxyBackend.__new__(NRProxyBackend)
        object.__setattr__(backend, "model_path", tmp_path / "fake.onnx")
        object.__setattr__(backend, "sidecar_path", None)
        object.__setattr__(backend, "use_gpu_ep", False)
        object.__setattr__(backend, "delta_fast", 8.0)
        object.__setattr__(backend, "_session", fake_session)
        object.__setattr__(backend, "_delta_fast_resolved", 8.0)
        object.__setattr__(backend, "_cache", {})

        r1 = backend.score_nr(yuv16x8, width=16, height=8)
        r2 = backend.score_nr(yuv32x16, width=32, height=16)
        assert r1.from_cache is False
        assert r2.from_cache is False
        assert len(fake_session._calls) == 2


# ---------------------------------------------------------------------------
# NRProxyBackend — model load error paths
# ---------------------------------------------------------------------------


class TestNRProxyBackendModelLoadErrors:
    def test_raises_when_model_file_missing(self, tmp_path):
        backend = NRProxyBackend(model_path=tmp_path / "missing.onnx")
        with pytest.raises(NRProxyBackendError, match="NR model file not found"):
            backend._get_session()

    def test_raises_when_onnxruntime_not_installed(self, tmp_path):
        model_path = tmp_path / "model.onnx"
        model_path.write_bytes(b"FAKE")
        backend = NRProxyBackend(model_path=model_path)
        # Temporarily remove onnxruntime from sys.modules if it exists.
        # We patch the import inside _load_ort_session.
        with mock.patch.dict("sys.modules", {"onnxruntime": None}):
            with pytest.raises(NRProxyBackendError, match="onnxruntime is required"):
                backend._get_session()


# ---------------------------------------------------------------------------
# NRProxyBackend — inference output validation
# ---------------------------------------------------------------------------


class TestNRProxyBackendInferenceValidation:
    def _make_session_with_output(self, score: float) -> _FakeOrtSession:
        return _FakeOrtSession(score=score)

    def _make_backend_with_session(self, tmp_path: Path, session: object) -> NRProxyBackend:
        backend = NRProxyBackend.__new__(NRProxyBackend)
        object.__setattr__(backend, "model_path", tmp_path / "fake.onnx")
        object.__setattr__(backend, "sidecar_path", None)
        object.__setattr__(backend, "use_gpu_ep", False)
        object.__setattr__(backend, "delta_fast", 8.0)
        object.__setattr__(backend, "_session", session)
        object.__setattr__(backend, "_delta_fast_resolved", 8.0)
        object.__setattr__(backend, "_calibration_slope_resolved", 1.0)
        object.__setattr__(backend, "_calibration_intercept_resolved", 0.0)
        object.__setattr__(backend, "_cache", {})
        return backend

    def test_valid_score_in_range(self, tmp_path):
        yuv = tmp_path / "dist.yuv"
        _write_synthetic_yuv(yuv, width=16, height=8, pix_fmt="yuv420p")
        session = self._make_session_with_output(72.0)
        backend = self._make_backend_with_session(tmp_path, session)
        result = backend.score_nr(yuv, width=16, height=8)
        assert 0.0 <= result.nr_score <= 100.0

    def test_out_of_range_score_raises(self, tmp_path):
        """A model returning >100 triggers NRProxyBackendError."""
        import numpy as np

        yuv = tmp_path / "dist.yuv"
        _write_synthetic_yuv(yuv, width=16, height=8, pix_fmt="yuv420p")

        class _BadSession:
            def get_inputs(self):
                return [SimpleNamespace(name="input")]

            def run(self, output_names, input_feed):
                return [np.array([[[[150.0]]]], dtype=np.float32)]

        backend = self._make_backend_with_session(tmp_path, _BadSession())
        with pytest.raises(NRProxyBackendError, match="out-of-range score"):
            backend.score_nr(yuv, width=16, height=8)

    def test_nan_score_raises(self, tmp_path):
        """A model returning NaN triggers NRProxyBackendError."""

        import numpy as np

        yuv = tmp_path / "dist.yuv"
        _write_synthetic_yuv(yuv, width=16, height=8, pix_fmt="yuv420p")

        class _NanSession:
            def get_inputs(self):
                return [SimpleNamespace(name="input")]

            def run(self, output_names, input_feed):
                return [np.array([[[[float("nan")]]]], dtype=np.float32)]

        backend = self._make_backend_with_session(tmp_path, _NanSession())
        with pytest.raises(NRProxyBackendError, match="out-of-range score"):
            backend.score_nr(yuv, width=16, height=8)


# ---------------------------------------------------------------------------
# NRProbeResult namedtuple
# ---------------------------------------------------------------------------


class TestNRProbeResult:
    def test_namedtuple_fields(self):
        r = NRProbeResult(nr_score=75.0, from_cache=False)
        assert r.nr_score == 75.0
        assert r.from_cache is False

    def test_cache_hit_result(self):
        r = NRProbeResult(nr_score=80.0, from_cache=True)
        assert r.from_cache is True


# ---------------------------------------------------------------------------
# GPU EP provider preference
# ---------------------------------------------------------------------------


class TestNRProxyBackendGpuEp:
    def test_cuda_ep_preferred_over_cpu_when_available(self, tmp_path):
        """When CUDAExecutionProvider is available it is prepended to providers."""
        model_path = tmp_path / "model.onnx"
        model_path.write_bytes(b"FAKE")

        captured_providers: list = []

        class _RecordingOrt:
            class SessionOptions:
                log_severity_level = 3

            @staticmethod
            def get_available_providers():
                return ["CUDAExecutionProvider", "CPUExecutionProvider"]

            class InferenceSession:
                def __init__(self, path, sess_options=None, providers=None):
                    captured_providers.extend(providers or [])

                def get_providers(self):
                    return captured_providers

        with mock.patch.dict("sys.modules", {"onnxruntime": _RecordingOrt()}):
            from vmaftune.score_backend import _load_ort_session

            try:
                _load_ort_session(model_path, use_gpu_ep=True)
            except Exception:
                pass  # InferenceSession stub may not run fully

        assert "CUDAExecutionProvider" in captured_providers
        assert captured_providers[0] == "CUDAExecutionProvider"

    def test_cpu_only_when_use_gpu_ep_false(self, tmp_path):
        """When use_gpu_ep=False, only CPUExecutionProvider is passed."""
        model_path = tmp_path / "model.onnx"
        model_path.write_bytes(b"FAKE")

        captured_providers: list = []

        class _RecordingOrt:
            class SessionOptions:
                log_severity_level = 3

            @staticmethod
            def get_available_providers():
                return ["CUDAExecutionProvider", "CPUExecutionProvider"]

            class InferenceSession:
                def __init__(self, path, sess_options=None, providers=None):
                    captured_providers.extend(providers or [])

                def get_providers(self):
                    return captured_providers

        with mock.patch.dict("sys.modules", {"onnxruntime": _RecordingOrt()}):
            from vmaftune.score_backend import _load_ort_session

            try:
                _load_ort_session(model_path, use_gpu_ep=False)
            except Exception:
                pass

        assert "CUDAExecutionProvider" not in captured_providers
        assert captured_providers == ["CPUExecutionProvider"]


# ---------------------------------------------------------------------------
# Integration: bisect NR early-elimination sentinel path
# ---------------------------------------------------------------------------


class TestBisectNRIntegration:
    """Test that the bisect loop correctly handles NR skip sentinels.

    Uses a stub NRProxyBackend (always returns 60.0, target=90.0, δ=8.0)
    together with a stub encode/score runner to exercise the sentinel path
    without real encodes or VMAF calls.
    """

    def test_nr_skip_sentinel_advances_window(self, tmp_path):
        """When NR score is far from target, FR is skipped and window advances."""

        from vmaftune.bisect import _try_nr_early_elimination_on_yuv

        # Synthetic distorted YUV
        yuv = tmp_path / "dist.yuv"
        _write_synthetic_yuv(yuv, width=16, height=8, pix_fmt="yuv420p")

        # Backend returning 60.0 when target=90.0 with δ=8.0 → far → skip FR
        session = _FakeOrtSession(score=60.0)
        backend = NRProxyBackend.__new__(NRProxyBackend)
        object.__setattr__(backend, "model_path", tmp_path / "fake.onnx")
        object.__setattr__(backend, "sidecar_path", None)
        object.__setattr__(backend, "use_gpu_ep", False)
        object.__setattr__(backend, "delta_fast", 8.0)
        object.__setattr__(backend, "_session", session)
        object.__setattr__(backend, "_delta_fast_resolved", 8.0)
        object.__setattr__(backend, "_calibration_slope_resolved", 1.0)
        object.__setattr__(backend, "_calibration_intercept_resolved", 0.0)
        object.__setattr__(backend, "_cache", {})

        result = _try_nr_early_elimination_on_yuv(
            nr_proxy_backend=backend,
            distorted_yuv=yuv,
            width=16,
            height=8,
            pix_fmt="yuv420p",
            target_vmaf=90.0,
        )
        assert result is not None
        direction, nr_score = result
        # NR=60 < target=90 → quality below target → need to compress less → "looser"
        assert direction == "looser"
        assert pytest.approx(nr_score, abs=0.01) == 60.0

    def test_nr_within_delta_returns_none(self, tmp_path):
        """When NR is within δ_fast the function returns None (pay FR)."""
        yuv = tmp_path / "dist.yuv"
        _write_synthetic_yuv(yuv, width=16, height=8, pix_fmt="yuv420p")

        # NR=88.0, target=90.0, δ=8.0: |88-90|=2 < 8 → within zone
        session = _FakeOrtSession(score=88.0)
        backend = NRProxyBackend.__new__(NRProxyBackend)
        object.__setattr__(backend, "model_path", tmp_path / "fake.onnx")
        object.__setattr__(backend, "sidecar_path", None)
        object.__setattr__(backend, "use_gpu_ep", False)
        object.__setattr__(backend, "delta_fast", 8.0)
        object.__setattr__(backend, "_session", session)
        object.__setattr__(backend, "_delta_fast_resolved", 8.0)
        object.__setattr__(backend, "_calibration_slope_resolved", 1.0)
        object.__setattr__(backend, "_calibration_intercept_resolved", 0.0)
        object.__setattr__(backend, "_cache", {})

        from vmaftune.bisect import _try_nr_early_elimination_on_yuv

        result = _try_nr_early_elimination_on_yuv(
            nr_proxy_backend=backend,
            distorted_yuv=yuv,
            width=16,
            height=8,
            pix_fmt="yuv420p",
            target_vmaf=90.0,
        )
        assert result is None

    def test_nr_inference_failure_falls_through_to_fr(self, tmp_path):
        """NR inference failure returns None so bisect falls through to FR."""
        yuv = tmp_path / "dist.yuv"
        _write_synthetic_yuv(yuv, width=16, height=8, pix_fmt="yuv420p")

        class _FailSession:
            def get_inputs(self):
                return [SimpleNamespace(name="input")]

            def run(self, output_names, input_feed):
                raise RuntimeError("inference exploded")

        backend = NRProxyBackend.__new__(NRProxyBackend)
        object.__setattr__(backend, "model_path", tmp_path / "fake.onnx")
        object.__setattr__(backend, "sidecar_path", None)
        object.__setattr__(backend, "use_gpu_ep", False)
        object.__setattr__(backend, "delta_fast", 8.0)
        object.__setattr__(backend, "_session", _FailSession())
        object.__setattr__(backend, "_delta_fast_resolved", 8.0)
        object.__setattr__(backend, "_calibration_slope_resolved", 1.0)
        object.__setattr__(backend, "_calibration_intercept_resolved", 0.0)
        object.__setattr__(backend, "_cache", {})

        from vmaftune.bisect import _try_nr_early_elimination_on_yuv

        result = _try_nr_early_elimination_on_yuv(
            nr_proxy_backend=backend,
            distorted_yuv=yuv,
            width=16,
            height=8,
            pix_fmt="yuv420p",
            target_vmaf=90.0,
        )
        # Failure → fall through → None
        assert result is None
