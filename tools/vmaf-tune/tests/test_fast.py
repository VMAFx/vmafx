# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Phase A.5 fast-path smoke + production-wiring tests
(ADR-0276 scaffold + ADR-0304 prod wiring).

Validates two surfaces:

1. The Optuna-driven smoke search loop end-to-end without needing
   ffmpeg, ONNX Runtime, or a GPU (scaffold contract from ADR-0276).
2. The production wiring seams: TPE → v2 proxy → GPU verify pass
   (ADR-0304). Each seam is tested with an injected fake so the
   suite runs on any host.
"""

from __future__ import annotations

import sys
import time
from pathlib import Path
from unittest.mock import MagicMock

import pytest

# Make src/ importable without an editable install.
_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

optuna = pytest.importorskip("optuna")

from vmaftune.fast import (
    DEFAULT_CRF_HI,
    DEFAULT_CRF_LO,
    DEFAULT_PROXY_TOLERANCE,
    PROD_N_TRIALS,
    SMOKE_N_TRIALS,
    TrialSample,
    fast_recommend,
)

# ---------------------------------------------------------------------------
# Smoke-mode tests — preserved from the ADR-0276 scaffold contract.
# ---------------------------------------------------------------------------


def test_smoke_recommendation_hits_target_within_tolerance() -> None:
    """Synthetic predictor + Optuna TPE should land within ≈1 VMAF of the target."""
    result = fast_recommend(src=None, target_vmaf=92.0, smoke=True)
    assert result["smoke"] is True
    assert result["encoder"] == "libx264"
    assert result["n_trials"] == SMOKE_N_TRIALS
    assert DEFAULT_CRF_LO <= result["recommended_crf"] <= DEFAULT_CRF_HI
    assert abs(result["predicted_vmaf"] - 92.0) < 1.5
    # Smoke mode never runs the verify pass.
    assert result["verify_vmaf"] is None
    assert result["proxy_verify_gap"] is None


def test_smoke_low_target_picks_higher_crf() -> None:
    """Lower VMAF target should map to higher CRF on the synthetic curve."""
    high_q = fast_recommend(src=None, target_vmaf=95.0, smoke=True)
    low_q = fast_recommend(src=None, target_vmaf=70.0, smoke=True)
    assert low_q["recommended_crf"] > high_q["recommended_crf"]
    assert low_q["predicted_kbps"] < high_q["predicted_kbps"]


def test_predictor_injection_drives_search() -> None:
    """A custom predictor closes the loop without needing smoke mode."""
    calls: list[int] = []

    def _flat_predictor(crf: int) -> TrialSample:
        calls.append(crf)
        return TrialSample(crf=crf, predicted_vmaf=88.0, predicted_kbps=float(60 - crf))

    def _fake_encode_runner(src: Path, encoder: str, crf: int, backend: str) -> tuple[float, float]:
        # Return (kbps, vmaf) with vmaf close to the proxy's 88.
        return (1500.0, 88.05)

    result = fast_recommend(
        src=Path("any.mp4"),
        target_vmaf=88.0,
        smoke=False,
        n_trials=10,
        predictor=_flat_predictor,
        encode_runner=_fake_encode_runner,
    )
    assert len(calls) > 0
    assert result["predicted_vmaf"] == 88.0
    assert DEFAULT_CRF_LO <= result["recommended_crf"] <= DEFAULT_CRF_HI
    assert result["verify_vmaf"] == pytest.approx(88.05)
    assert result["proxy_verify_gap"] is not None
    assert result["proxy_verify_gap"] < DEFAULT_PROXY_TOLERANCE


def test_crf_range_is_respected() -> None:
    result = fast_recommend(
        src=None,
        target_vmaf=85.0,
        smoke=True,
        crf_range=(20, 30),
        n_trials=20,
    )
    assert 20 <= result["recommended_crf"] <= 30


def test_time_budget_stops_tpe_before_requested_trials() -> None:
    """``--time-budget-s`` is a real Optuna timeout, not just metadata."""

    def _slow_predictor(crf: int) -> TrialSample:
        time.sleep(0.02)
        return TrialSample(crf=crf, predicted_vmaf=90.0, predicted_kbps=2000.0)

    result = fast_recommend(
        src=None,
        target_vmaf=90.0,
        smoke=True,
        predictor=_slow_predictor,
        n_trials=100,
        time_budget_s=0.05,
    )
    assert 1 <= result["n_trials"] < 100


# ---------------------------------------------------------------------------
# ADR-0304 production-wiring tests — TPE / proxy / verify seams.
# ---------------------------------------------------------------------------


def test_production_loop_uses_real_extractor_when_no_override() -> None:
    """`smoke=False` without any override wires the production extractor.

    The production extractor calls ffprobe on the source; when the
    source does not exist the extractor raises RuntimeError (propagated
    from the ffprobe failure), not NotImplementedError.  A caller that
    wants full control injects ``sample_extractor`` or ``predictor``.
    """
    with pytest.raises(RuntimeError, match="ffprobe failed"):
        fast_recommend(src=Path("nonexistent.mp4"), target_vmaf=92.0, smoke=False)


def test_tpe_search_smoke_uses_prod_default_when_unset() -> None:
    """In production mode without n_trials override, default is PROD_N_TRIALS."""

    def _flat(crf: int) -> TrialSample:
        return TrialSample(crf=crf, predicted_vmaf=90.0, predicted_kbps=2000.0)

    def _fake_runner(src: Path, encoder: str, crf: int, backend: str) -> tuple[float, float]:
        return (2000.0, 90.0)

    result = fast_recommend(
        src=Path("ignored.mp4"),
        target_vmaf=90.0,
        smoke=False,
        predictor=_flat,
        encode_runner=_fake_runner,
    )
    assert result["n_trials"] == PROD_N_TRIALS
    assert result["smoke"] is False


def test_proxy_score_calls_v2_session(monkeypatch: pytest.MonkeyPatch) -> None:
    """The production predictor seam must call vmaftune.proxy.run_proxy.

    We monkey-patch ``vmaftune.proxy.run_proxy`` to a recording fake,
    then build the production predictor via ``sample_extractor`` and
    confirm Optuna drives the proxy through the seam (not directly via
    onnxruntime). This is the contract the ADR-0304 fast-path proxy
    invariant pins.
    """
    proxy_module = pytest.importorskip("vmaftune.proxy")

    captured: list[dict] = []

    def _fake_run_proxy(
        features,
        *,
        encoder: str,
        preset_norm: float,
        crf_norm: float,
        **_kwargs,
    ) -> float:
        captured.append(
            {
                "features": list(features),
                "encoder": encoder,
                "preset_norm": preset_norm,
                "crf_norm": crf_norm,
            }
        )
        # Return a deterministic VMAF that depends on crf_norm so TPE
        # has an objective.
        return 100.0 - 30.0 * crf_norm

    monkeypatch.setattr(proxy_module, "run_proxy", _fake_run_proxy)

    def _fake_extractor(src: Path, crf: int, encoder: str) -> tuple[list[float], float]:
        # Six canonical-6 features (post-scaler).
        return ([0.5, 0.4, 0.3, 0.2, 0.1, 0.05], float(8000 - 100 * crf))

    def _fake_runner(src: Path, encoder: str, crf: int, backend: str) -> tuple[float, float]:
        return (2000.0, 85.0)

    result = fast_recommend(
        src=Path("any.mp4"),
        target_vmaf=85.0,
        smoke=False,
        n_trials=8,
        sample_extractor=_fake_extractor,
        encode_runner=_fake_runner,
    )
    assert result["smoke"] is False
    # Proxy was called for each TPE trial.
    assert len(captured) >= 1
    assert all(c["encoder"] == "libx264" for c in captured)
    assert all(0.0 <= c["crf_norm"] <= 1.0 for c in captured)
    assert all(len(c["features"]) == 6 for c in captured)


def test_gpu_verify_pass_at_end() -> None:
    """The verify pass must run exactly once at the end of the search.

    Proxy alone never wins (ADR-0304 invariant). This test confirms
    that ``encode_runner`` is invoked exactly once after TPE completes
    and that the verify_vmaf field reflects its return value.
    """
    runner_calls: list[tuple] = []

    def _flat_predictor(crf: int) -> TrialSample:
        # Proxy thinks every CRF gives 92.0.
        return TrialSample(crf=crf, predicted_vmaf=92.0, predicted_kbps=1800.0)

    def _runner(src: Path, encoder: str, crf: int, backend: str) -> tuple[float, float]:
        runner_calls.append((str(src), encoder, crf, backend))
        # Real libvmaf disagrees with the proxy: returns 89.5.
        return (1800.0, 89.5)

    result = fast_recommend(
        src=Path("source.mp4"),
        target_vmaf=92.0,
        smoke=False,
        n_trials=5,
        predictor=_flat_predictor,
        encode_runner=_runner,
    )
    # Exactly one verify pass.
    assert len(runner_calls) == 1
    assert result["verify_vmaf"] == pytest.approx(89.5)
    # Gap = |92.0 - 89.5| = 2.5, exceeds the 1.5 default tolerance.
    assert result["proxy_verify_gap"] == pytest.approx(2.5)
    assert "FLAG" in result["notes"]


def test_gpu_verify_within_tolerance_no_flag() -> None:
    """When proxy and verify agree, no OOD flag in notes."""

    def _good_predictor(crf: int) -> TrialSample:
        return TrialSample(crf=crf, predicted_vmaf=90.0, predicted_kbps=1500.0)

    def _runner(src: Path, encoder: str, crf: int, backend: str) -> tuple[float, float]:
        # Verify says 90.3 — gap of 0.3, well within 1.5 tolerance.
        return (1500.0, 90.3)

    result = fast_recommend(
        src=Path("source.mp4"),
        target_vmaf=90.0,
        smoke=False,
        n_trials=5,
        predictor=_good_predictor,
        encode_runner=_runner,
    )
    assert result["proxy_verify_gap"] == pytest.approx(0.3, abs=1e-6)
    assert "FLAG" not in result["notes"]


def test_proxy_module_uses_lazy_import_seam() -> None:
    """vmaftune.proxy.run_proxy must accept a session_factory test seam.

    Tests should be able to inject a fake InferenceSession-shaped
    object without ever importing onnxruntime. This confirms the
    ADR-0304 single-seam discipline.

    The v2 ONNX graph is exported with two *separate* named inputs —
    "features" (shape [N, 6]) and "codec" (shape [N, 14]) — matching the
    FRRegressor.forward(features, codec_onehot) signature in
    ai/src/vmaf_train/models/fr_regressor.py.  The session_factory seam
    must expose both inputs so run_proxy can wire them individually.
    """
    proxy_module = pytest.importorskip("vmaftune.proxy")
    pytest.importorskip("numpy")  # proxy needs numpy

    captured_inputs: list = []

    class _FakeSession:
        """Two-input fake that mirrors the real fr_regressor_v2 ONNX graph."""

        def get_inputs(self) -> list:
            feat_inp = MagicMock()
            feat_inp.name = "features"
            codec_inp = MagicMock()
            codec_inp.name = "codec"
            return [feat_inp, codec_inp]

        def run(self, output_names, input_feed):
            captured_inputs.append(input_feed)
            import numpy as _np

            return [_np.asarray([[88.5]], dtype=_np.float32)]

    def _factory(_path):
        return _FakeSession()

    score = proxy_module.run_proxy(
        [0.1, 0.2, 0.3, 0.4, 0.5, 0.6],
        encoder="libx264",
        preset_norm=0.5,
        crf_norm=0.3,
        session_factory=_factory,
    )
    assert score == pytest.approx(88.5)
    # The corrected behaviour: two *separate* tensors, not one concatenated 20-D blob.
    assert len(captured_inputs) == 1
    feed = captured_inputs[0]
    assert set(feed.keys()) == {"features", "codec"}, (
        "run_proxy must pass two named tensors to the two-input v2 graph; "
        f"got keys: {set(feed.keys())}"
    )
    assert feed["features"].shape == (
        1,
        6,
    ), f"features tensor must be (1, 6); got {feed['features'].shape}"
    assert feed["codec"].shape == (
        1,
        14,
    ), f"codec tensor must be (1, 14); got {feed['codec'].shape}"
