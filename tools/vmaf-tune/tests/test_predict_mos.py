# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Predictor MOS surface — Phase 3 of ADR-0325.

Pins the contract exposed by ``Predictor.predict_mos``:

* When ``model/konvid_mos_head_v1.onnx`` ships and ``onnxruntime`` is
  available, the call routes through the head and returns its
  prediction (clamped to ``[1.0, 5.0]``).
* When the ONNX is missing, the call falls back to the documented
  linear approximation ``mos = (predicted_vmaf - 30) / 14`` clamped to
  ``[1, 5]`` — never raises.
* The MOS surface accepts the same :class:`ShotFeatures` shape the
  VMAF surface consumes, so call sites can switch between the two
  without re-extracting features.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune import predictor as predictor_mod  # noqa: E402
from vmaftune.predictor import Predictor, ShotFeatures  # noqa: E402


@pytest.fixture(autouse=True)
def _reset_mos_head_cache():
    """Force a fresh resolve before and after each test.

    The MOS-head loader is module-level cached for production performance;
    the test harness needs to clear it so each case sees the file-system
    state set up by that case (or its absence).
    """
    predictor_mod._reset_mos_head_cache_for_tests()
    yield
    predictor_mod._reset_mos_head_cache_for_tests()


def _basic_features(saliency: float = 0.3, motion: float = 5.0) -> ShotFeatures:
    return ShotFeatures(
        probe_bitrate_kbps=2000.0,
        probe_i_frame_avg_bytes=10000.0,
        probe_p_frame_avg_bytes=2000.0,
        probe_b_frame_avg_bytes=0.0,
        saliency_mean=saliency,
        saliency_var=saliency * 0.1,
        frame_diff_mean=motion,
        y_avg=128.0,
        y_var=400.0,
        shot_length_frames=120,
        fps=30.0,
        width=1920,
        height=1080,
    )


# ---------------------------------------------------------------------
# Surface contract — the method exists with the documented signature.
# ---------------------------------------------------------------------


def test_predictor_exposes_predict_mos() -> None:
    p = Predictor()
    assert hasattr(p, "predict_mos")
    assert callable(p.predict_mos)


# ---------------------------------------------------------------------
# Fallback path — no ONNX file present, no onnxruntime needed.
# ---------------------------------------------------------------------


def test_predict_mos_falls_back_when_onnx_missing(monkeypatch: pytest.MonkeyPatch) -> None:
    """When ``konvid_mos_head_v1.onnx`` is absent, the linear fallback fires.

    Pin the resolver to ``None`` directly — the test cannot rely on
    the real on-disk file being present or absent; it must control
    the resolver outcome explicitly.
    """
    monkeypatch.setattr(predictor_mod, "_resolve_mos_head_path", lambda: None)
    p = Predictor()
    f = _basic_features()
    mos = p.predict_mos(f, codec="libx264")
    assert isinstance(mos, float)
    # Fallback is `(vmaf - 30) / 14` clamped to [1, 5]. For libx264 at
    # the default CRF the analytical predictor returns ~95 → mos ~4.6.
    assert 1.0 <= mos <= 5.0


def test_predict_mos_fallback_passes_target_quality_through(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Caller-supplied ``target_quality`` reaches the underlying VMAF predictor."""
    monkeypatch.setattr(predictor_mod, "_resolve_mos_head_path", lambda: None)
    p = Predictor()
    f = _basic_features()
    # Higher CRF → lower VMAF → lower MOS in the fallback path.
    mos_low_crf = p.predict_mos(f, codec="libx264", target_quality=18)
    mos_high_crf = p.predict_mos(f, codec="libx264", target_quality=40)
    assert mos_low_crf > mos_high_crf


def test_predict_mos_fallback_clamps_low_quality(monkeypatch: pytest.MonkeyPatch) -> None:
    """Catastrophically bad VMAF predictions clamp to MOS 1.0, not below."""
    monkeypatch.setattr(predictor_mod, "_resolve_mos_head_path", lambda: None)
    p = Predictor()
    f = _basic_features()
    # CRF 51 is the libx264 worst-case quality knob; the analytical
    # fallback's quadratic term drives predicted VMAF deeply negative,
    # which the (vmaf - 30) / 14 mapping sends well below MOS 1. The
    # clamp must catch it.
    mos = p.predict_mos(f, codec="libx264", target_quality=51)
    assert mos >= 1.0


def test_predict_mos_fallback_works_without_onnxruntime(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Hosts without onnxruntime still get a valid MOS estimate.

    Pin the loader to None to simulate the import-failure branch
    without uninstalling onnxruntime from the test environment.
    """
    monkeypatch.setattr(predictor_mod, "_maybe_load_mos_head", lambda: None)
    p = Predictor()
    f = _basic_features()
    mos = p.predict_mos(f, codec="libx264")
    assert 1.0 <= mos <= 5.0


# ---------------------------------------------------------------------
# Head path — when the real ONNX is on disk the call routes to it.
# ---------------------------------------------------------------------


def test_predict_mos_routes_through_head_when_session_present(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """A real onnxruntime InferenceSession over the real shipped ONNX.

    Skips when either the ONNX file is missing (fork-local dev branch)
    or onnxruntime is not installed.
    """
    pytest.importorskip("onnxruntime")
    onnx_path = Path(__file__).resolve().parents[3] / "model" / "konvid_mos_head_v1.onnx"
    if not onnx_path.is_file():
        pytest.skip(f"konvid_mos_head_v1.onnx not present at {onnx_path}")
    monkeypatch.setattr(predictor_mod, "_resolve_mos_head_path", lambda: onnx_path)
    p = Predictor()
    f = _basic_features(saliency=0.5)
    mos = p.predict_mos(f, codec="libx264")
    assert 1.0 <= mos <= 5.0


def test_predict_mos_uses_head_when_session_object_returned(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Stubbed session object — no real ONNX file required.

    Inject a duck-typed session that returns a fixed [3.7] tensor and
    verify the predictor routes the call through it (not the
    fallback) and that the output is honoured.
    """

    class _StubSession:
        def get_inputs(self):
            class _Input:
                def __init__(self, name: str) -> None:
                    self.name = name

            return [_Input("features"), _Input("encoder_onehot")]

        def run(self, _output_names, _feed):
            import numpy as np

            return [np.asarray([[3.7]], dtype=np.float32)]

    monkeypatch.setattr(predictor_mod, "_maybe_load_mos_head", lambda: _StubSession())
    p = Predictor()
    f = _basic_features(saliency=0.5)
    mos = p.predict_mos(f, codec="libx264")
    assert mos == pytest.approx(3.7, abs=1e-5)


def test_predict_mos_head_clamps_out_of_range(monkeypatch: pytest.MonkeyPatch) -> None:
    """Even an adversarial head output is clamped to ``[1.0, 5.0]``."""

    class _AdversarialSession:
        def get_inputs(self):
            class _Input:
                def __init__(self, name: str) -> None:
                    self.name = name

            return [_Input("features"), _Input("encoder_onehot")]

        def run(self, _output_names, _feed):
            import numpy as np

            return [np.asarray([[42.0]], dtype=np.float32)]

    monkeypatch.setattr(predictor_mod, "_maybe_load_mos_head", lambda: _AdversarialSession())
    p = Predictor()
    f = _basic_features()
    mos = p.predict_mos(f, codec="libx264")
    assert mos == pytest.approx(5.0)
