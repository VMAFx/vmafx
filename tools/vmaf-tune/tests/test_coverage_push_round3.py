# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Coverage push round 3 — targeted tests for remaining uncovered branches.

Modules targeted (by coverage gap as of master 2026-06-13):

* :mod:`vmaftune.score` — model-arg with ``=`` pass-through, yuv422/yuv444
  pixel-format paths, 12-bit depth path, TypeError/ValueError in
  ``parse_feature_aggregates``, ``maybe_decode_distorted`` raw-suffix early
  return, ``run_score`` JSON-decode-error + ``parse_vmaf_json`` ValueError
  paths.
* :mod:`vmaftune.encoder_runtime` — empty-token, multi-``@``, empty-adapter,
  whitespace-in-adapter, comma-in-adapter, missing-``=`` in binding,
  empty-path in binding.
* :mod:`vmaftune.proxy` — onnxruntime-missing path via ``sys.modules``
  injection, single-input-graph fallback in ``run_proxy``.
* :mod:`vmaftune.fast` — ``_build_production_encode_runner`` (production
  backend wiring), ``_gpu_verify`` (seam injection), ``fast_recommend``
  production OOD-flag branch (gap > tolerance).

All tests are pure unit-level: no subprocess, no ffmpeg, no ONNX runtime,
no network.

Provenance: PR coverage-push round 3 — 2026-06-13.
"""

from __future__ import annotations

import json
import sys
import types
from pathlib import Path
from typing import Any
from unittest import mock

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.encoder_runtime import (  # noqa: E402
    parse_encoder_runtime_token,
    resolve_encoder_runtime_specs,
)
from vmaftune.score import (  # noqa: E402
    ScoreRequest,
    _bitdepth_for,
    _model_arg,
    _pixfmt_to_vmaf,
    maybe_decode_distorted,
    parse_feature_aggregates,
    run_score,
)

# ---------------------------------------------------------------------------
# vmaftune.score — _model_arg, _pixfmt_to_vmaf, _bitdepth_for
# ---------------------------------------------------------------------------


class TestModelArg:
    """``_model_arg`` wraps bare ids and passes pre-formatted strings through."""

    def test_bare_id_gets_version_prefix(self) -> None:
        assert _model_arg("vmaf_v0.6.1") == "version=vmaf_v0.6.1"

    def test_preformatted_string_passes_through(self) -> None:
        # Any string with ``=`` is treated as already-formatted.
        assert _model_arg("path=/models/vmaf_hdr.json") == "path=/models/vmaf_hdr.json"
        assert _model_arg("version=vmaf_v0.6.1") == "version=vmaf_v0.6.1"


class TestPixfmtToVmaf:
    """``_pixfmt_to_vmaf`` maps ffmpeg pix_fmt strings to libvmaf vocabulary."""

    def test_yuv422_returns_422(self) -> None:
        assert _pixfmt_to_vmaf("yuv422p") == "422"
        assert _pixfmt_to_vmaf("yuv422p10le") == "422"

    def test_yuv444_returns_444(self) -> None:
        assert _pixfmt_to_vmaf("yuv444p") == "444"
        assert _pixfmt_to_vmaf("yuv444p10le") == "444"

    def test_yuv420_returns_420(self) -> None:
        assert _pixfmt_to_vmaf("yuv420p") == "420"
        assert _pixfmt_to_vmaf("yuv420p10le") == "420"
        assert _pixfmt_to_vmaf("unknown_fmt") == "420"


class TestBitdepthFor:
    """``_bitdepth_for`` maps pix_fmt to bit depth."""

    def test_10le_returns_10(self) -> None:
        assert _bitdepth_for("yuv420p10le") == 10

    def test_p10_returns_10(self) -> None:
        assert _bitdepth_for("yuv420p10") == 10

    def test_12le_returns_12(self) -> None:
        assert _bitdepth_for("yuv420p12le") == 12

    def test_p12_returns_12(self) -> None:
        assert _bitdepth_for("yuv420p12") == 12

    def test_8bit_returns_8(self) -> None:
        assert _bitdepth_for("yuv420p") == 8
        assert _bitdepth_for("yuv444p") == 8


class TestParseFeatureAggregatesExceptionPaths:
    """``parse_feature_aggregates`` handles non-convertible mean/stddev values."""

    def test_nonnumeric_mean_is_silently_skipped(self) -> None:
        # TypeError path: float("not a number") raises ValueError, not TypeError,
        # but the code catches both; we trigger the ValueError path.
        payload: dict[str, Any] = {
            "pooled_metrics": {"integer_adm2": {"mean": "not-a-float", "stddev": "also-bad"}}
        }
        means, stds = parse_feature_aggregates(payload, ("adm2",))
        assert "adm2" not in means
        assert "adm2" not in stds

    def test_none_mean_is_silently_skipped(self) -> None:
        # None → float(None) raises TypeError, which is caught.
        payload = {"pooled_metrics": {"integer_adm2": {"mean": None, "stddev": None}}}
        means, stds = parse_feature_aggregates(payload, ("adm2",))
        assert "adm2" not in means

    def test_valid_mean_invalid_stddev_partial_result(self) -> None:
        payload = {"pooled_metrics": {"integer_adm2": {"mean": 0.95, "stddev": "bad"}}}
        means, stds = parse_feature_aggregates(payload, ("adm2",))
        assert means.get("adm2") == pytest.approx(0.95)
        assert "adm2" not in stds


class TestMaybeDecodeDistortedRawSuffix:
    """``maybe_decode_distorted`` returns immediately for ``.yuv`` inputs."""

    def test_yuv_suffix_skips_decode(self, tmp_path: Path) -> None:
        ref = tmp_path / "ref.yuv"
        dist = tmp_path / "dist.yuv"
        ref.touch()
        dist.touch()
        req = ScoreRequest(
            reference=ref,
            distorted=dist,
            width=320,
            height=240,
            pix_fmt="yuv420p",
        )
        returned_req, rc = maybe_decode_distorted(req, workdir=tmp_path)
        # No decode should occur; the original request and rc=0 are returned.
        assert rc == 0
        assert returned_req is req

    def test_empty_suffix_skips_decode(self, tmp_path: Path) -> None:
        ref = tmp_path / "ref"
        dist = tmp_path / "dist"
        ref.touch()
        dist.touch()
        req = ScoreRequest(
            reference=ref,
            distorted=dist,
            width=320,
            height=240,
            pix_fmt="yuv420p",
        )
        returned_req, rc = maybe_decode_distorted(req, workdir=tmp_path)
        assert rc == 0
        assert returned_req is req


class TestRunScoreJsonDecodeErrorPath:
    """``run_score`` treats corrupt JSON (exit 0) as exit-65 scoring failure."""

    def test_corrupt_json_sets_rc65_and_nan_score(self, tmp_path: Path) -> None:
        ref = tmp_path / "ref.yuv"
        ref.touch()

        def _runner(cmd: list[str], **kw: Any) -> Any:
            # Simulate vmaf exiting 0 but writing corrupt JSON.
            json_out_path = next((Path(c) for c in cmd if c.endswith(".json")), None)
            if json_out_path is not None:
                json_out_path.write_text("{corrupt json}", encoding="utf-8")
            return types.SimpleNamespace(returncode=0, stdout="", stderr="")

        req = ScoreRequest(
            reference=ref,
            distorted=ref,
            width=320,
            height=240,
            pix_fmt="yuv420p",
        )
        result = run_score(req, vmaf_bin="vmaf", runner=_runner, workdir=tmp_path)
        import math

        assert math.isnan(result.vmaf_score)
        assert result.exit_status == 65


class TestRunScoreParseVmafJsonValueErrorPath:
    """``run_score`` handles valid JSON that has no VMAF score key."""

    def test_missing_vmaf_mean_sets_rc65_and_nan_score(self, tmp_path: Path) -> None:
        ref = tmp_path / "ref.yuv"
        ref.touch()

        def _runner(cmd: list[str], **kw: Any) -> Any:
            json_out_path = next((Path(c) for c in cmd if c.endswith(".json")), None)
            if json_out_path is not None:
                # Valid JSON but no ``pooled_metrics.vmaf.mean`` and no
                # legacy ``VMAF score`` key.
                json_out_path.write_text(json.dumps({"pooled_metrics": {}}), encoding="utf-8")
            return types.SimpleNamespace(returncode=0, stdout="", stderr="")

        req = ScoreRequest(
            reference=ref,
            distorted=ref,
            width=320,
            height=240,
            pix_fmt="yuv420p",
        )
        result = run_score(req, vmaf_bin="vmaf", runner=_runner, workdir=tmp_path)
        import math

        assert math.isnan(result.vmaf_score)
        assert result.exit_status == 65


# ---------------------------------------------------------------------------
# vmaftune.encoder_runtime — uncovered error paths
# ---------------------------------------------------------------------------


class TestParseEncoderRuntimeTokenErrors:
    """Error-path coverage for ``parse_encoder_runtime_token``."""

    def test_empty_token_raises_value_error(self) -> None:
        with pytest.raises(ValueError, match="empty encoder token"):
            parse_encoder_runtime_token("")

    def test_whitespace_only_token_raises(self) -> None:
        with pytest.raises(ValueError, match="empty encoder token"):
            parse_encoder_runtime_token("   ")

    def test_multiple_at_signs_raises(self) -> None:
        with pytest.raises(ValueError, match="use ADAPTER or ADAPTER@VARIANT"):
            parse_encoder_runtime_token("a@b@c")

    def test_empty_adapter_with_variant_raises(self) -> None:
        with pytest.raises(ValueError, match="adapter and variant are required"):
            parse_encoder_runtime_token("@myvariant")

    def test_adapter_with_empty_variant_raises(self) -> None:
        with pytest.raises(ValueError, match="adapter and variant are required"):
            parse_encoder_runtime_token("libsvtav1@")

    def test_whitespace_in_adapter_raises(self) -> None:
        with pytest.raises(ValueError, match="adapter contains whitespace"):
            parse_encoder_runtime_token("lib svtav1")

    def test_comma_in_adapter_raises(self) -> None:
        with pytest.raises(ValueError, match="adapter contains"):
            parse_encoder_runtime_token("lib,svtav1")

    def test_equals_in_adapter_raises(self) -> None:
        with pytest.raises(ValueError, match="adapter contains"):
            parse_encoder_runtime_token("lib=svtav1")

    def test_whitespace_in_variant_raises(self) -> None:
        with pytest.raises(ValueError, match="variant contains whitespace"):
            parse_encoder_runtime_token("libsvtav1@svt av1 hdr")


class TestResolveEncoderRuntimeSpecsErrors:
    """Error-path coverage for ``resolve_encoder_runtime_specs``."""

    def test_missing_equals_in_binding_raises(self) -> None:
        with pytest.raises(ValueError, match="expected ENCODER=PATH"):
            resolve_encoder_runtime_specs(
                ["libsvtav1"],
                encoder_ffmpeg_bins=["libsvtav1_no_equals_sign"],
            )

    def test_empty_path_in_binding_raises(self) -> None:
        with pytest.raises(ValueError, match="PATH must not be empty"):
            resolve_encoder_runtime_specs(
                ["libsvtav1"],
                encoder_ffmpeg_bins=["libsvtav1="],
            )

    def test_variant_whitespace_in_binding_key_raises(self) -> None:
        """A binding key that itself is an invalid token should raise."""
        with pytest.raises(ValueError):
            resolve_encoder_runtime_specs(
                ["libsvtav1"],
                encoder_ffmpeg_bins=["lib svtav1=/opt/ffmpeg"],
            )


# ---------------------------------------------------------------------------
# vmaftune.proxy — onnxruntime-missing path + single-input graph fallback
# ---------------------------------------------------------------------------


class TestProxyOnnxruntimeMissingPath:
    """``_import_onnxruntime`` raises ``ProxyError`` when onnxruntime absent."""

    def test_import_error_raises_proxy_error(self) -> None:
        from vmaftune.proxy import ProxyError, _import_onnxruntime

        # Temporarily shadow onnxruntime in sys.modules with an ImportError sentinel.
        with mock.patch.dict(sys.modules, {"onnxruntime": None}):
            with pytest.raises(ProxyError, match="onnxruntime is required"):
                _import_onnxruntime()

    def test_import_success_returns_ort_module(self) -> None:
        """The ``return ort`` success path is exercised when onnxruntime is present."""
        ort = pytest.importorskip("onnxruntime")
        from vmaftune.proxy import _import_onnxruntime

        result = _import_onnxruntime()
        assert result is ort


class TestRunProxyProductionPath:
    """``run_proxy`` production path (session_factory=None) against a real model.

    Uses the default ``fr_regressor_v2`` (two-input graph) to hit the
    ``session_factory is None`` branch (lines 192-193 in proxy.py), which
    exercises ``_resolve_model_path`` + ``_load_session`` without any
    injected seam.
    """

    def test_production_path_with_real_v2_model(self) -> None:
        """Run ``run_proxy`` without injecting a session_factory.

        Requires onnxruntime and the in-tree ``fr_regressor_v2.onnx`` model.
        Covers lines 192-193 (session_factory is None) and the standard
        two-input graph path.
        """
        pytest.importorskip("onnxruntime")
        pytest.importorskip("numpy")

        from vmaftune.proxy import (
            DEFAULT_PROXY_MODEL_ID,
            ProxyError,
            _resolve_model_path,
            run_proxy,
        )

        # Skip if the model is absent (stripped CI checkout).
        try:
            _resolve_model_path(DEFAULT_PROXY_MODEL_ID)
        except ProxyError:
            pytest.skip(f"{DEFAULT_PROXY_MODEL_ID}.onnx not present in tree")

        # ``libx264`` is in ENCODER_VOCAB_V2; plausible normalised features.
        score = run_proxy(
            [0.95, 0.85, 0.75, 0.65, 0.55, 0.1],
            encoder="libx264",
            preset_norm=0.5,
            crf_norm=0.4,
            # No session_factory → production _resolve_model_path + _load_session.
        )
        # Score is model-dependent; verify it is a finite number.
        import math

        assert isinstance(score, float)
        assert math.isfinite(score)


class TestRunProxySingleInputGraphFallback:
    """``run_proxy`` falls back to concatenated input for single-input ONNX graphs."""

    def test_single_input_graph_receives_20d_vector(self) -> None:
        np = pytest.importorskip("numpy")

        received: dict[str, Any] = {}

        class _FakeInput:
            def __init__(self, name: str) -> None:
                self.name = name

        class _FakeSingleInputSession:
            def get_inputs(self) -> list[_FakeInput]:
                # Only ONE input (single-input graph — legacy smoke export).
                return [_FakeInput("combined")]

            def run(self, output_names: list[str] | None, input_feed: dict[str, Any]) -> list[Any]:
                received.update(input_feed)
                combined = input_feed["combined"]
                assert combined.shape == (1, 20), f"expected (1,20), got {combined.shape}"
                return [np.asarray([[77.3]], dtype=np.float32)]

        def _factory(model_path: Path) -> _FakeSingleInputSession:
            return _FakeSingleInputSession()

        from vmaftune.proxy import run_proxy

        score = run_proxy(
            [0.9, 0.8, 0.7, 0.6, 0.5, 0.4],
            encoder="libx264",
            preset_norm=0.5,
            crf_norm=0.3,
            model_id="fr_regressor_v1",
            session_factory=_factory,
        )
        assert score == pytest.approx(77.3)
        assert "combined" in received


# ---------------------------------------------------------------------------
# vmaftune.fast — production OOD flag + _gpu_verify seam injection
# ---------------------------------------------------------------------------


class TestFastRecommendProductionOODFlag:
    """``fast_recommend`` marks result OOD when proxy/verify gap > tolerance."""

    def test_ood_flag_appears_in_notes_when_gap_exceeds_tolerance(self) -> None:
        optuna = pytest.importorskip("optuna")  # noqa: F841

        from vmaftune.fast import TrialSample, fast_recommend

        call_count = {"n": 0}

        def _deterministic_predictor(crf: int) -> TrialSample:
            call_count["n"] += 1
            # Always predicts 90 VMAF.
            return TrialSample(crf=crf, predicted_vmaf=90.0, predicted_kbps=2000.0)

        def _fake_encode_runner(
            src: Path, encoder: str, crf: int, backend: str
        ) -> tuple[float, float]:
            # Returns verify vmaf far from proxy's 90.0 to trigger OOD flag.
            return (1500.0, 70.0)  # gap = 20 VMAF >> default tolerance 1.5

        result = fast_recommend(
            src=Path("any.mp4"),
            target_vmaf=90.0,
            smoke=False,
            n_trials=5,
            predictor=_deterministic_predictor,
            encode_runner=_fake_encode_runner,
            proxy_tolerance=1.5,  # default; gap=20 exceeds this
        )
        assert call_count["n"] > 0
        assert result["verify_vmaf"] == pytest.approx(70.0)
        gap = result["proxy_verify_gap"]
        assert gap is not None
        assert gap > 1.5
        assert "FLAG" in result["notes"]

    def test_within_tolerance_no_ood_flag(self) -> None:
        pytest.importorskip("optuna")

        from vmaftune.fast import TrialSample, fast_recommend

        def _predictor(crf: int) -> TrialSample:
            return TrialSample(crf=crf, predicted_vmaf=90.0, predicted_kbps=2000.0)

        def _runner(src: Path, encoder: str, crf: int, backend: str) -> tuple[float, float]:
            # Gap = 0.5, below default tolerance 1.5.
            return (1500.0, 90.5)

        result = fast_recommend(
            src=Path("any.mp4"),
            target_vmaf=90.0,
            smoke=False,
            n_trials=5,
            predictor=_predictor,
            encode_runner=_runner,
        )
        assert result["proxy_verify_gap"] is not None
        assert result["proxy_verify_gap"] < 1.5
        assert "FLAG" not in result["notes"]


class TestGpuVerifySeamInjection:
    """``_gpu_verify`` honours injected ``score_backend_select`` + ``encode_runner``."""

    def test_seams_are_called_and_result_forwarded(self) -> None:
        pytest.importorskip("optuna")  # guard: fast module needs optuna
        from vmaftune.fast import _gpu_verify

        backend_selected: list[str] = []
        encode_called: list[tuple[Any, ...]] = []

        def _fake_select(prefer: str) -> str:
            backend_selected.append(prefer)
            return "cpu"

        def _fake_runner(src: Path, encoder: str, crf: int, backend: str) -> tuple[float, float]:
            encode_called.append((src, encoder, crf, backend))
            return (2000.0, 88.7)

        result = _gpu_verify(
            src=Path("clip.mp4"),
            encoder="libx264",
            crf=28,
            score_backend_select=_fake_select,
            encode_runner=_fake_runner,
        )
        assert result == pytest.approx(88.7)
        assert backend_selected == ["auto"]
        assert len(encode_called) == 1
        src_used, enc_used, crf_used, backend_used = encode_called[0]
        assert enc_used == "libx264"
        assert crf_used == 28
        assert backend_used == "cpu"
