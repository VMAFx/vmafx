# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Targeted coverage push for the lowest-covered vmaf-tune modules.

Single-file aggregator of focused unit tests that exercise the
uncovered branches identified by the coverage report on master
(2026-05-31). Each test class targets one module's documented gaps:

* :mod:`vmaftune.uncertainty` — sidecar loader failure modes,
  ``ConfidenceThresholds`` invariant raises, ``classify_interval``
  negative-width guard.
* :mod:`vmaftune.codec_adapters._gop_common` — ``default_gop_args``
  invariant raises (keyint, min_keyint).
* :mod:`vmaftune.proxy` — onnxruntime-missing path, OOD-encoder
  guard, canonical-6 length contract, ``_resolve_model_path``
  not-found raise.
* :mod:`vmaftune.predictor_features` — ``_probe_video_geometry``
  failure paths (rc!=0, malformed JSON, missing streams) +
  ``_parse_fps`` edge cases + ``_run_probe_encode`` short-circuit
  (frames<=0) + ``_run_signalstats`` short-circuit + ``_mean``
  empty-list.
* :mod:`vmaftune.benchmark` — ``render_benchmark`` unknown format,
  ``_resolve_baseline`` missing-encoder raise, encode/score FPS
  helpers' invalid-input branches, ``summarize_benchmark`` empty
  / no-encoder failure paths.
* :mod:`vmaftune.encoder_profile` — error paths: unreadable file,
  malformed JSON, schema mismatch, missing recommendations list,
  HTML without <pre>, recommendation index out of range, no codec
  on recommendation, no usable CRF, raw source missing
  width/height/fps, ``_default_preset`` for unknown codec, source
  auto-detect on raw suffixes.
* :mod:`vmaftune.fast` — ``_require_optuna`` raise (when Optuna
  unavailable; skipped when present), ``_run_tpe`` time_budget
  validation, ``fast_recommend(src=None)`` ValueError in
  production mode, smoke-mode path with a custom predictor.

All tests are pure unit-level: no subprocess spawn, no ffmpeg, no
ONNX runtime, no network. Each test mocks the relevant seam.

Provenance: PR #<TBD> — vmaf-tune coverage push, 2026-05-31.
"""

from __future__ import annotations

import json
import logging
import subprocess
import sys
from pathlib import Path
from typing import Any

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.benchmark import (  # noqa: E402
    _resolve_baseline,
    _row_encode_fps,
    _row_score_fps,
    render_benchmark,
    summarize_benchmark,
)
from vmaftune.codec_adapters._gop_common import (  # noqa: E402
    default_force_keyframes_args,
    default_gop_args,
    default_probe_args,
)
from vmaftune.encoder_profile import (  # noqa: E402
    build_encode_request,
    infer_source_is_container,
    load_profile_payload,
    select_recommendation,
)
from vmaftune.per_shot import Shot  # noqa: E402
from vmaftune.predictor_features import (  # noqa: E402
    FeatureExtractorConfig,
    _mean,
    _parse_fps,
    _parse_frame_sizes,
    _probe_video_geometry,
    _run_probe_encode,
    _run_signalstats,
)
from vmaftune.proxy import (  # noqa: E402
    ENCODER_VOCAB_V2,
    ProxyError,
    _resolve_model_path,
    encode_codec_block,
    run_proxy,
)
from vmaftune.uncertainty import (  # noqa: E402
    DEFAULT_TIGHT_INTERVAL_MAX_WIDTH,
    DEFAULT_WIDE_INTERVAL_MIN_WIDTH,
    ConfidenceDecision,
    ConfidenceThresholds,
    classify_interval,
    interval_excludes_target,
    load_confidence_thresholds,
)

# ---------------------------------------------------------------------------
# vmaftune.uncertainty
# ---------------------------------------------------------------------------


class TestUncertaintyLoaderFailures:
    """Covers the four documented failure modes of ``load_confidence_thresholds``.

    Each path logs a WARNING and returns documented defaults rather
    than raising — keeps a missing sidecar from killing the run.
    """

    def test_none_sidecar_returns_defaults_and_warns(
        self, caplog: pytest.LogCaptureFixture
    ) -> None:
        with caplog.at_level(logging.WARNING, logger="vmaftune.uncertainty"):
            out = load_confidence_thresholds(None)
        assert out.tight_interval_max_width == DEFAULT_TIGHT_INTERVAL_MAX_WIDTH
        assert out.wide_interval_min_width == DEFAULT_WIDE_INTERVAL_MIN_WIDTH
        assert out.source == "default"
        assert any("no calibration sidecar" in r.message for r in caplog.records)

    def test_missing_path_returns_defaults_and_warns(
        self, tmp_path: Path, caplog: pytest.LogCaptureFixture
    ) -> None:
        missing = tmp_path / "does-not-exist.json"
        with caplog.at_level(logging.WARNING, logger="vmaftune.uncertainty"):
            out = load_confidence_thresholds(missing)
        assert out.source == "default"
        assert any("not found" in r.message for r in caplog.records)

    def test_unreadable_json_returns_defaults_and_warns(
        self, tmp_path: Path, caplog: pytest.LogCaptureFixture
    ) -> None:
        bad = tmp_path / "bad.json"
        bad.write_text("{ this is not json", encoding="utf-8")
        with caplog.at_level(logging.WARNING, logger="vmaftune.uncertainty"):
            out = load_confidence_thresholds(bad)
        assert out.source == "default"
        assert any("unreadable" in r.message for r in caplog.records)

    def test_missing_keys_returns_defaults_and_warns(
        self, tmp_path: Path, caplog: pytest.LogCaptureFixture
    ) -> None:
        bad = tmp_path / "missing.json"
        bad.write_text(json.dumps({"tight_interval_max_width": 1.0}), encoding="utf-8")
        with caplog.at_level(logging.WARNING, logger="vmaftune.uncertainty"):
            out = load_confidence_thresholds(bad)
        assert out.source == "default"

    def test_invariant_violation_in_sidecar_returns_defaults(
        self, tmp_path: Path, caplog: pytest.LogCaptureFixture
    ) -> None:
        # tight > wide is rejected by ConfidenceThresholds; the loader
        # catches the ValueError and falls back to defaults.
        bad = tmp_path / "swapped.json"
        bad.write_text(
            json.dumps({"tight_interval_max_width": 10.0, "wide_interval_min_width": 1.0}),
            encoding="utf-8",
        )
        with caplog.at_level(logging.WARNING, logger="vmaftune.uncertainty"):
            out = load_confidence_thresholds(bad)
        assert out.source == "default"

    def test_valid_sidecar_overrides_defaults(self, tmp_path: Path) -> None:
        good = tmp_path / "good.json"
        good.write_text(
            json.dumps({"tight_interval_max_width": 1.6, "wide_interval_min_width": 4.2}),
            encoding="utf-8",
        )
        out = load_confidence_thresholds(good)
        assert out.tight_interval_max_width == pytest.approx(1.6)
        assert out.wide_interval_min_width == pytest.approx(4.2)
        assert out.source == str(good)


class TestConfidenceThresholdsInvariants:
    """``ConfidenceThresholds.__post_init__`` rejects nonsense pairs."""

    def test_negative_tight_raises(self) -> None:
        with pytest.raises(ValueError, match="must be positive"):
            ConfidenceThresholds(tight_interval_max_width=-1.0, wide_interval_min_width=5.0)

    def test_zero_wide_raises(self) -> None:
        with pytest.raises(ValueError, match="must be positive"):
            ConfidenceThresholds(tight_interval_max_width=1.0, wide_interval_min_width=0.0)

    def test_tight_exceeds_wide_raises(self) -> None:
        with pytest.raises(ValueError, match="<= wide_interval_min_width"):
            ConfidenceThresholds(tight_interval_max_width=8.0, wide_interval_min_width=5.0)

    def test_tight_equal_wide_is_accepted(self) -> None:
        # Boundary case: tight == wide collapses MIDDLE to empty, both
        # allowed by the invariant.
        out = ConfidenceThresholds(tight_interval_max_width=3.0, wide_interval_min_width=3.0)
        assert out.tight_interval_max_width == 3.0


class TestClassifyIntervalEdges:
    """``classify_interval`` covers the three bands + NaN + negative."""

    def test_nan_returns_middle(self) -> None:
        out = classify_interval(float("nan"), ConfidenceThresholds())
        assert out == ConfidenceDecision.MIDDLE

    def test_negative_raises(self) -> None:
        with pytest.raises(ValueError, match=">= 0.0 or NaN"):
            classify_interval(-1.0, ConfidenceThresholds())

    def test_tight_band_boundary(self) -> None:
        thresholds = ConfidenceThresholds(tight_interval_max_width=2.0, wide_interval_min_width=5.0)
        assert classify_interval(2.0, thresholds) == ConfidenceDecision.TIGHT
        assert classify_interval(1.99, thresholds) == ConfidenceDecision.TIGHT

    def test_wide_band_boundary(self) -> None:
        thresholds = ConfidenceThresholds(tight_interval_max_width=2.0, wide_interval_min_width=5.0)
        assert classify_interval(5.0, thresholds) == ConfidenceDecision.WIDE
        assert classify_interval(99.9, thresholds) == ConfidenceDecision.WIDE

    def test_middle_band(self) -> None:
        thresholds = ConfidenceThresholds(tight_interval_max_width=2.0, wide_interval_min_width=5.0)
        assert classify_interval(3.5, thresholds) == ConfidenceDecision.MIDDLE


class TestIntervalExcludesTargetExtras:
    """``interval_excludes_target`` non-finite + slack branches."""

    def test_non_finite_low_returns_false(self) -> None:
        assert not interval_excludes_target(low=float("inf"), high=80.0, target=90.0)

    def test_non_finite_high_returns_false(self) -> None:
        assert not interval_excludes_target(low=80.0, high=float("nan"), target=90.0)

    def test_slack_widens_miss_zone(self) -> None:
        # Without slack: interval [88, 89] excludes target 90.
        assert interval_excludes_target(low=88.0, high=89.0, target=90.0)
        # With slack=2: interval needs to be below target-slack=88, so
        # the same interval no longer excludes.
        assert not interval_excludes_target(low=88.0, high=89.0, target=90.0, slack=2.0)


# ---------------------------------------------------------------------------
# vmaftune.codec_adapters._gop_common
# ---------------------------------------------------------------------------


class TestDefaultGopArgsInvariants:
    """``default_gop_args`` enforces keyint / min_keyint invariants."""

    def test_keyint_one_is_minimum(self) -> None:
        assert default_gop_args(1) == ("-g", "1")

    def test_keyint_zero_raises(self) -> None:
        with pytest.raises(ValueError, match="keyint must be >= 1"):
            default_gop_args(0)

    def test_keyint_negative_raises(self) -> None:
        with pytest.raises(ValueError, match="keyint must be >= 1"):
            default_gop_args(-5)

    def test_min_keyint_zero_raises(self) -> None:
        with pytest.raises(ValueError, match="min_keyint must be in"):
            default_gop_args(60, min_keyint=0)

    def test_min_keyint_exceeds_keyint_raises(self) -> None:
        with pytest.raises(ValueError, match="min_keyint must be in"):
            default_gop_args(60, min_keyint=100)

    def test_min_keyint_equal_keyint_is_allowed(self) -> None:
        out = default_gop_args(60, min_keyint=60)
        assert out == ("-g", "60", "-keyint_min", "60")

    def test_force_keyframes_empty(self) -> None:
        assert default_force_keyframes_args(()) == ()

    def test_force_keyframes_microsecond_precision(self) -> None:
        out = default_force_keyframes_args((0.5, 1.2345678))
        # Six-decimal precision per the docstring contract.
        assert out == ("-force_key_frames", "0.500000,1.234568")

    def test_default_probe_args_delegates_to_adapter(self) -> None:
        class _FakeAdapter:
            probe_preset = "ultrafast"
            probe_quality = 30

            def ffmpeg_codec_args(self, preset: str, quality: int) -> list[str]:
                assert preset == "ultrafast"
                assert quality == 30
                return ["-preset", preset, "-crf", str(quality)]

        out = default_probe_args(_FakeAdapter())
        assert out == ["-preset", "ultrafast", "-crf", "30"]


# ---------------------------------------------------------------------------
# vmaftune.proxy
# ---------------------------------------------------------------------------


class TestProxyContracts:
    """``run_proxy`` + ``encode_codec_block`` + ``_resolve_model_path`` guards."""

    def test_ood_encoder_raises(self) -> None:
        with pytest.raises(ProxyError, match="not in ENCODER_VOCAB_V2"):
            encode_codec_block("libxyz_unknown", preset_norm=0.5, crf_norm=0.5)

    def test_vocab_is_frozen_length_12(self) -> None:
        # ADR-0291 contract: 12-way codec one-hot.
        assert len(ENCODER_VOCAB_V2) == 12

    def test_encode_codec_block_returns_14d(self) -> None:
        np = pytest.importorskip("numpy")
        block = encode_codec_block("libx264", preset_norm=0.5, crf_norm=0.3)
        assert block.shape == (14,)
        # First slot of one-hot = libx264 (vocab order).
        assert block[ENCODER_VOCAB_V2.index("libx264")] == pytest.approx(1.0)
        # Trailing two slots are preset_norm + crf_norm.
        assert block[12] == pytest.approx(0.5)
        assert block[13] == pytest.approx(0.3)
        assert block.dtype == np.float32

    def test_run_proxy_rejects_non_canonical_6_features(self) -> None:
        pytest.importorskip("numpy")
        with pytest.raises(ProxyError, match="canonical-6"):
            run_proxy(
                [0.1, 0.2, 0.3],  # only 3 values, not 6
                encoder="libx264",
                preset_norm=0.5,
                crf_norm=0.5,
                session_factory=lambda _p: None,
            )

    def test_resolve_model_path_raises_when_missing(self) -> None:
        with pytest.raises(ProxyError, match="could not locate"):
            _resolve_model_path("nonexistent_model_xyz_12345")

    def test_run_proxy_through_injected_session_factory(self) -> None:
        np = pytest.importorskip("numpy")

        class _FakeInput:
            name = "input"

        class _FakeSession:
            def get_inputs(self) -> list[_FakeInput]:
                return [_FakeInput()]

            def run(self, output_names: Any, input_feed: dict[str, Any]) -> list[Any]:
                # The combined feature+codec block must be 20-D.
                arr = input_feed["input"]
                assert arr.shape == (1, 20)
                return [np.asarray([[88.5]], dtype=np.float32)]

        def _factory(model_path: Path) -> _FakeSession:
            # The real path resolver isn't usable in a test, so callers
            # injecting a factory expect _resolve_model_path to raise
            # before factory invocation if the model is absent. Use a
            # model id that exists in-tree (fr_regressor_v1 ships).
            assert model_path.exists()
            return _FakeSession()

        score = run_proxy(
            [0.1, 0.2, 0.3, 0.4, 0.5, 0.6],
            encoder="libx264",
            preset_norm=0.5,
            crf_norm=0.5,
            model_id="fr_regressor_v1",
            session_factory=_factory,
        )
        assert score == pytest.approx(88.5)


# ---------------------------------------------------------------------------
# vmaftune.predictor_features (internals)
# ---------------------------------------------------------------------------


def _stub_run(returncode: int, stdout: str = "", stderr: str = "") -> Any:
    """Build a stub subprocess.run that returns CompletedProcess fields."""

    def _run(cmd: list[str], **_kwargs: Any) -> subprocess.CompletedProcess[str]:
        return subprocess.CompletedProcess(cmd, returncode, stdout=stdout, stderr=stderr)

    return _run


class TestProbeVideoGeometry:
    """``_probe_video_geometry`` failure paths."""

    def test_rc_nonzero_returns_zeros(self, tmp_path: Path) -> None:
        cfg = FeatureExtractorConfig(ffprobe_bin="ffprobe-test")
        out = _probe_video_geometry(tmp_path / "src.mp4", cfg, _stub_run(1))
        assert out == (0, 0, 0.0)

    def test_invalid_json_returns_zeros(self, tmp_path: Path) -> None:
        cfg = FeatureExtractorConfig(ffprobe_bin="ffprobe-test")
        out = _probe_video_geometry(tmp_path / "src.mp4", cfg, _stub_run(0, stdout="not json"))
        assert out == (0, 0, 0.0)

    def test_missing_streams_returns_zeros(self, tmp_path: Path) -> None:
        cfg = FeatureExtractorConfig(ffprobe_bin="ffprobe-test")
        out = _probe_video_geometry(
            tmp_path / "src.mp4", cfg, _stub_run(0, stdout=json.dumps({"streams": []}))
        )
        assert out == (0, 0, 0.0)

    def test_well_formed_payload_parses(self, tmp_path: Path) -> None:
        cfg = FeatureExtractorConfig(ffprobe_bin="ffprobe-test")
        payload = json.dumps(
            {"streams": [{"width": 1920, "height": 1080, "r_frame_rate": "24000/1001"}]}
        )
        out = _probe_video_geometry(tmp_path / "src.mp4", cfg, _stub_run(0, stdout=payload))
        assert out[0] == 1920
        assert out[1] == 1080
        assert out[2] == pytest.approx(24000 / 1001)


class TestParseFpsEdges:
    """``_parse_fps`` handles malformed strings without raising."""

    def test_no_slash_parses_as_float(self) -> None:
        assert _parse_fps("60") == pytest.approx(60.0)

    def test_no_slash_invalid_returns_zero(self) -> None:
        assert _parse_fps("abc") == 0.0

    def test_invalid_numerator_returns_zero(self) -> None:
        assert _parse_fps("abc/1") == 0.0

    def test_invalid_denominator_returns_zero(self) -> None:
        assert _parse_fps("60/xyz") == 0.0

    def test_zero_denominator_returns_zero(self) -> None:
        assert _parse_fps("60/0") == 0.0

    def test_negative_denominator_returns_zero(self) -> None:
        assert _parse_fps("60/-1") == 0.0


class TestRunProbeEncodeShortCircuits:
    """``_run_probe_encode`` zero-frame and rc-failure short-circuits."""

    def test_zero_frames_returns_default(self, tmp_path: Path) -> None:
        cfg = FeatureExtractorConfig(probe_max_frames=0)
        shot = Shot(0, 10)
        # Should never call run because frames<=0.
        called: list[Any] = []

        def _runner(*_a: Any, **_kw: Any) -> Any:
            called.append(1)
            raise AssertionError("should not be called")

        out = _run_probe_encode(shot, tmp_path / "src.mp4", "libx264", cfg, _runner)
        assert out.bitrate_kbps == 0.0
        assert called == []

    def test_nonzero_rc_returns_default(self, tmp_path: Path) -> None:
        cfg = FeatureExtractorConfig(ffmpeg_bin="ffmpeg-test")
        shot = Shot(0, 10)
        out = _run_probe_encode(shot, tmp_path / "src.mp4", "libx264", cfg, _stub_run(1))
        assert out.bitrate_kbps == 0.0
        assert out.i_frame_avg_bytes == 0.0


class TestParseFrameSizes:
    """``_parse_frame_sizes`` from vstats file content."""

    def test_missing_file_returns_zeros(self, tmp_path: Path) -> None:
        out = _parse_frame_sizes(tmp_path / "absent.txt")
        assert out == (0.0, 0.0, 0.0)

    def test_well_formed_vstats_averages_per_type(self, tmp_path: Path) -> None:
        vstats = tmp_path / "vstats.txt"
        vstats.write_text(
            "frame=    1 type= I size= 1000 q=0.0\n"
            "frame=    2 type= P size= 500 q=0.0\n"
            "frame=    3 type= P size= 700 q=0.0\n"
            "frame=    4 type= B size= 200 q=0.0\n",
            encoding="utf-8",
        )
        i_avg, p_avg, b_avg = _parse_frame_sizes(vstats)
        assert i_avg == pytest.approx(1000.0)
        assert p_avg == pytest.approx(600.0)  # (500+700)/2
        assert b_avg == pytest.approx(200.0)

    def test_unknown_type_ignored(self, tmp_path: Path) -> None:
        vstats = tmp_path / "vstats.txt"
        vstats.write_text("frame=1 type= Z size= 999\n", encoding="utf-8")
        assert _parse_frame_sizes(vstats) == (0.0, 0.0, 0.0)

    def test_missing_size_field_ignored(self, tmp_path: Path) -> None:
        vstats = tmp_path / "vstats.txt"
        vstats.write_text("frame=1 type= I no_size_here\n", encoding="utf-8")
        assert _parse_frame_sizes(vstats) == (0.0, 0.0, 0.0)


class TestRunSignalstats:
    """``_run_signalstats`` short-circuits + rc-failure path."""

    def test_zero_frames_returns_default(self, tmp_path: Path) -> None:
        cfg = FeatureExtractorConfig(probe_max_frames=0)
        out = _run_signalstats(Shot(0, 10), tmp_path / "src.mp4", cfg, _stub_run(0))
        assert out.y_avg == 0.0

    def test_nonzero_rc_returns_default(self, tmp_path: Path) -> None:
        cfg = FeatureExtractorConfig(ffmpeg_bin="ffmpeg-test")
        out = _run_signalstats(Shot(0, 10), tmp_path / "src.mp4", cfg, _stub_run(1))
        assert out.y_avg == 0.0


class TestMeanHelper:
    """``_mean`` empty-list short-circuit."""

    def test_empty_returns_zero(self) -> None:
        assert _mean([]) == 0.0

    def test_arithmetic_mean(self) -> None:
        assert _mean([1.0, 2.0, 3.0]) == pytest.approx(2.0)


# ---------------------------------------------------------------------------
# vmaftune.benchmark
# ---------------------------------------------------------------------------


def _bench_row(
    *,
    encoder: str = "libx264",
    crf: int = 28,
    vmaf: float = 92.0,
    bitrate: float = 2000.0,
    exit_status: int = 0,
    duration_s: float | None = 10.0,
    encode_time_ms: float | None = 5000.0,
    score_time_ms: float | None = 2000.0,
    framerate: float | None = 24.0,
) -> dict:
    row: dict = {
        "src": "clip_a.yuv",
        "encoder": encoder,
        "preset": "medium",
        "crf": crf,
        "vmaf_score": vmaf,
        "bitrate_kbps": bitrate,
        "exit_status": exit_status,
        "vmaf_model": "vmaf_v0.6.1",
    }
    if duration_s is not None:
        row["duration_s"] = duration_s
    if encode_time_ms is not None:
        row["encode_time_ms"] = encode_time_ms
    if score_time_ms is not None:
        row["score_time_ms"] = score_time_ms
    if framerate is not None:
        row["framerate"] = framerate
    return row


class TestBenchmarkFpsHelpers:
    """``_row_encode_fps`` / ``_row_score_fps`` reject malformed rows."""

    def test_encode_fps_missing_duration(self) -> None:
        assert _row_encode_fps(_bench_row(duration_s=None)) is None

    def test_encode_fps_zero_encode_time(self) -> None:
        assert _row_encode_fps(_bench_row(encode_time_ms=0.0)) is None

    def test_encode_fps_zero_duration(self) -> None:
        assert _row_encode_fps(_bench_row(duration_s=0.0)) is None

    def test_encode_fps_well_formed(self) -> None:
        # 10s / 5s = 2 fps speedup.
        out = _row_encode_fps(_bench_row(duration_s=10.0, encode_time_ms=5000.0))
        assert out == pytest.approx(2.0)

    def test_score_fps_missing_framerate(self) -> None:
        assert _row_score_fps(_bench_row(framerate=None)) is None

    def test_score_fps_zero_framerate(self) -> None:
        assert _row_score_fps(_bench_row(framerate=0.0)) is None

    def test_score_fps_well_formed(self) -> None:
        # (10s * 24fps) / 2s = 120 fps.
        out = _row_score_fps(_bench_row(duration_s=10.0, score_time_ms=2000.0, framerate=24.0))
        assert out == pytest.approx(120.0)


class TestSummarizeBenchmarkFailures:
    """``summarize_benchmark`` raises on empty / no-encoder corpora."""

    def test_empty_corpus_raises(self) -> None:
        with pytest.raises(ValueError, match="no successful finite corpus"):
            summarize_benchmark([], target_vmaf=90.0)

    def test_all_failures_raises(self) -> None:
        with pytest.raises(ValueError, match="no successful finite corpus"):
            summarize_benchmark([_bench_row(encoder="libx264", exit_status=1)], target_vmaf=90.0)

    def test_rows_without_encoder_raises(self) -> None:
        row = _bench_row(encoder="")
        with pytest.raises(ValueError, match="do not include encoder names"):
            summarize_benchmark([row], target_vmaf=90.0)


class TestResolveBaselineRaises:
    """``_resolve_baseline`` rejects an unknown baseline encoder."""

    def test_missing_baseline_raises(self) -> None:
        rows = [_bench_row(encoder="libx264", vmaf=95.0)]
        summaries = summarize_benchmark(rows, target_vmaf=90.0)
        with pytest.raises(ValueError, match="not present in corpus"):
            _resolve_baseline(summaries, "libsvtav1")

    def test_no_clearing_encoder_returns_none(self) -> None:
        # Every encoder fails to clear; baseline resolution should pick None.
        rows = [_bench_row(encoder="libx264", vmaf=50.0)]
        summaries = summarize_benchmark(rows, target_vmaf=90.0)
        assert _resolve_baseline(summaries, None) is None


class TestRenderBenchmarkUnknownFormat:
    """``render_benchmark`` rejects unknown formats."""

    def test_unknown_format_raises(self) -> None:
        rows = [_bench_row(encoder="libx264", vmaf=95.0)]
        summaries = summarize_benchmark(rows, target_vmaf=90.0)
        with pytest.raises(ValueError, match="unknown benchmark format"):
            render_benchmark(summaries, fmt="yaml")


# ---------------------------------------------------------------------------
# vmaftune.encoder_profile
# ---------------------------------------------------------------------------


_VALID_PROFILE: dict[str, Any] = {
    "schema": "vmaftune.encoder_profile.v1",
    "source": {
        "path": "/profiles/source.mkv",
        "width": 1920,
        "height": 1080,
        "fps": 24.0,
        "duration_s": 10.0,
    },
    "run": {"pix_fmt": "yuv420p", "preset": "medium"},
    "recommendations": [
        {
            "codec": "libx265",
            "crf": 24,
            "bitrate_kbps": 2200,
            "target_vmaf": 94.0,
            "selected_pareto": True,
            "preset": "medium",
        },
        {
            "codec": "libsvtav1",
            "crf": 30,
            "bitrate_kbps": 1600,
            "target_vmaf": 94.0,
            "selected_pareto": True,
            "preset": "medium",
        },
    ],
}


class TestLoadProfilePayloadErrors:
    """``load_profile_payload`` surface errors on every failure mode."""

    def test_unreadable_path_raises(self, tmp_path: Path) -> None:
        missing = tmp_path / "ghost.json"
        with pytest.raises(ValueError, match="cannot read profile"):
            load_profile_payload(missing)

    def test_malformed_json_raises(self, tmp_path: Path) -> None:
        bad = tmp_path / "bad.json"
        bad.write_text("{ this is not json", encoding="utf-8")
        with pytest.raises(ValueError, match="cannot parse profile JSON"):
            load_profile_payload(bad)

    def test_html_without_pre_block_raises(self, tmp_path: Path) -> None:
        bad = tmp_path / "no-pre.html"
        bad.write_text("<html><body>no payload here</body></html>", encoding="utf-8")
        with pytest.raises(ValueError, match="does not contain a raw JSON"):
            load_profile_payload(bad)

    def test_markdown_without_fenced_json_raises(self, tmp_path: Path) -> None:
        bad = tmp_path / "plain.md"
        bad.write_text("# Just a heading", encoding="utf-8")
        with pytest.raises(ValueError, match="does not contain a fenced JSON"):
            load_profile_payload(bad)

    def test_wrong_schema_raises(self, tmp_path: Path) -> None:
        bad = tmp_path / "wrong_schema.json"
        bad.write_text(
            json.dumps({"schema": "something.else.v1", "recommendations": []}),
            encoding="utf-8",
        )
        with pytest.raises(ValueError, match="unsupported encoder profile schema"):
            load_profile_payload(bad)

    def test_missing_recommendations_raises(self, tmp_path: Path) -> None:
        bad = tmp_path / "no_recs.json"
        bad.write_text(json.dumps({"schema": "vmaftune.encoder_profile.v1"}), encoding="utf-8")
        with pytest.raises(ValueError, match="no recommendations list"):
            load_profile_payload(bad)


class TestSelectRecommendationFilters:
    """``select_recommendation`` filter + index branches."""

    def test_codec_filter_no_match_raises(self) -> None:
        with pytest.raises(ValueError, match="no recommendation matching"):
            select_recommendation(_VALID_PROFILE, codec="libvvenc")

    def test_target_vmaf_filter_no_match_raises(self) -> None:
        with pytest.raises(ValueError, match="no recommendation matching"):
            select_recommendation(_VALID_PROFILE, target_vmaf=200.0)

    def test_index_out_of_range_raises(self) -> None:
        with pytest.raises(ValueError, match="outside filtered range"):
            select_recommendation(_VALID_PROFILE, recommendation_index=99)

    def test_negative_index_raises(self) -> None:
        with pytest.raises(ValueError, match="outside filtered range"):
            select_recommendation(_VALID_PROFILE, recommendation_index=-1)

    def test_pareto_sort_prefers_lower_bitrate(self) -> None:
        # Both rows are Pareto-selected; lowest bitrate wins (libsvtav1).
        rec = select_recommendation(_VALID_PROFILE, target_vmaf=94.0)
        assert rec["codec"] == "libsvtav1"

    def test_index_zero_returns_top_after_sort(self) -> None:
        rec = select_recommendation(_VALID_PROFILE, recommendation_index=0)
        assert rec["codec"] == "libsvtav1"


class TestBuildEncodeRequestErrors:
    """``build_encode_request`` validation raises."""

    def test_missing_codec_raises(self, tmp_path: Path) -> None:
        rec: dict[str, Any] = {"codec": "", "crf": 24}
        with pytest.raises(ValueError, match="no codec"):
            build_encode_request(_VALID_PROFILE, rec, output=tmp_path / "out.mkv")

    def test_missing_crf_raises(self, tmp_path: Path) -> None:
        rec: dict[str, Any] = {"codec": "libx265"}
        with pytest.raises(ValueError, match="no usable CRF"):
            build_encode_request(_VALID_PROFILE, rec, output=tmp_path / "out.mkv")

    def test_raw_source_without_geometry_raises(self, tmp_path: Path) -> None:
        rec = _VALID_PROFILE["recommendations"][0]
        # Profile reports no width/height/fps when stripped; raw .yuv
        # source therefore needs the operator to pass overrides.
        bare_profile: dict[str, Any] = {
            "schema": "vmaftune.encoder_profile.v1",
            "source": {"path": str(tmp_path / "src.yuv")},
            "run": {},
            "recommendations": [rec],
        }
        with pytest.raises(ValueError, match="raw sources require"):
            build_encode_request(
                bare_profile,
                rec,
                output=tmp_path / "out.mkv",
                source_override=tmp_path / "src.yuv",
            )

    def test_adapter_default_preset_replaces_sentinel(self, tmp_path: Path) -> None:
        rec = dict(_VALID_PROFILE["recommendations"][0])
        rec["preset"] = "adapter default"
        req = build_encode_request(
            _VALID_PROFILE,
            rec,
            output=tmp_path / "out.mkv",
            source_override=tmp_path / "input.mkv",
        )
        assert req.preset != "adapter default"
        # The chosen default is the adapter's medium preset (x265 supports it).
        assert req.preset == "medium"

    def test_duration_override_propagates(self, tmp_path: Path) -> None:
        rec = _VALID_PROFILE["recommendations"][0]
        req = build_encode_request(
            _VALID_PROFILE,
            rec,
            output=tmp_path / "out.mkv",
            source_override=tmp_path / "input.mkv",
            duration_override=42.0,
        )
        assert req.duration_s == pytest.approx(42.0)


class TestInferSourceContainer:
    """``infer_source_is_container`` source-kind branches."""

    def test_explicit_container(self) -> None:
        assert infer_source_is_container(Path("x.yuv"), source_kind="container") is True

    def test_explicit_raw(self) -> None:
        assert infer_source_is_container(Path("x.mkv"), source_kind="raw") is False

    def test_auto_recognizes_yuv_suffix(self) -> None:
        assert infer_source_is_container(Path("x.yuv"), source_kind="auto") is False
        assert infer_source_is_container(Path("x.raw"), source_kind="auto") is False

    def test_auto_recognizes_container_suffix(self) -> None:
        assert infer_source_is_container(Path("x.mkv"), source_kind="auto") is True
        assert infer_source_is_container(Path("x.mp4"), source_kind="auto") is True

    def test_unknown_kind_raises(self) -> None:
        with pytest.raises(ValueError, match="unknown source kind"):
            infer_source_is_container(Path("x.mkv"), source_kind="bogus")


# ---------------------------------------------------------------------------
# vmaftune.fast
# ---------------------------------------------------------------------------


class TestFastRecommendValidation:
    """``fast_recommend`` argument validation + smoke entry."""

    def test_production_mode_requires_src(self) -> None:
        optuna = pytest.importorskip("optuna")  # noqa: F841  (sentinel)
        from vmaftune.fast import fast_recommend

        with pytest.raises(ValueError, match="requires a source path"):
            fast_recommend(None, target_vmaf=90.0, smoke=False, n_trials=2)

    def test_run_tpe_rejects_zero_time_budget(self) -> None:
        pytest.importorskip("optuna")
        from vmaftune.fast import TrialSample, _run_tpe

        def _pred(crf: int) -> TrialSample:
            return TrialSample(crf=crf, predicted_vmaf=90.0, predicted_kbps=2000.0)

        with pytest.raises(ValueError, match="time_budget_s must be > 0"):
            _run_tpe(
                target_vmaf=90.0,
                predictor=_pred,
                crf_range=(10, 30),
                n_trials=1,
                time_budget_s=0.0,
            )

    def test_run_tpe_rejects_negative_time_budget(self) -> None:
        pytest.importorskip("optuna")
        from vmaftune.fast import TrialSample, _run_tpe

        def _pred(crf: int) -> TrialSample:
            return TrialSample(crf=crf, predicted_vmaf=90.0, predicted_kbps=2000.0)

        with pytest.raises(ValueError, match="time_budget_s must be > 0"):
            _run_tpe(
                target_vmaf=90.0,
                predictor=_pred,
                crf_range=(10, 30),
                n_trials=1,
                time_budget_s=-5.0,
            )

    def test_smoke_mode_with_custom_predictor(self) -> None:
        pytest.importorskip("optuna")
        from vmaftune.fast import TrialSample, fast_recommend

        # Custom predictor steers TPE toward CRF 25.
        def _pred(crf: int) -> TrialSample:
            quality_gap = abs(crf - 25)
            return TrialSample(
                crf=crf,
                predicted_vmaf=95.0 - quality_gap,
                predicted_kbps=2000.0 + quality_gap * 50.0,
            )

        result = fast_recommend(
            None,
            target_vmaf=95.0,
            smoke=True,
            predictor=_pred,
            n_trials=5,
            crf_range=(20, 30),
        )
        assert result["smoke"] is True
        assert result["verify_vmaf"] is None
        assert result["proxy_verify_gap"] is None
        assert 20 <= result["recommended_crf"] <= 30
