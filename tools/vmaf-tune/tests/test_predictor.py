# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Per-shot VMAF predictor — unit tests.

Pins:

* The analytical fallback predictor is monotone-decreasing in CRF for
  every shipped codec.
* ``pick_crf`` returns a CRF inside the codec's ``quality_range`` and
  is monotone in target VMAF (higher target → lower CRF, more bits).
* ``pick_keyint`` branches correctly across the three motion bands
  (low-motion-long → 4×fps; high-motion → fps; default → 2×fps).
* ``select_validation_shots`` stratifies by probe-bitrate quartile.
* ``validate_predictor`` walks GOSPEL / RECALIBRATE / FALL_BACK by
  injecting deterministic residuals.
* ``predictor_features`` parsers handle FFmpeg stderr / vstats / lavfi
  metadata in their published shapes.

All subprocess calls are stubbed; the suite passes with no ffmpeg /
onnxruntime installed.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.codec_adapters import known_codecs  # noqa: E402
from vmaftune.per_shot import Shot  # noqa: E402
from vmaftune.predictor import (  # noqa: E402
    Predictor,
    ShotFeatures,
    make_predictor_predicate,
    pick_keyint,
    resolution_class,
)
from vmaftune.predictor_features import (  # noqa: E402
    FeatureExtractorConfig,
    _compute_saliency,
    _parse_bitrate,
    _parse_metadata_float,
    _parse_signalstats,
)
from vmaftune.predictor_validate import (  # noqa: E402
    ShotResidual,
    Verdict,
    _decide_verdict,
    select_validation_shots,
    validate_predictor,
)

# --- ShotFeatures ----------------------------------------------------


def test_shot_features_is_frozen_and_takes_defaults():
    f = ShotFeatures(
        probe_bitrate_kbps=2000.0,
        probe_i_frame_avg_bytes=10000.0,
        probe_p_frame_avg_bytes=2000.0,
        probe_b_frame_avg_bytes=0.0,
    )
    assert f.saliency_mean == 0.0
    with pytest.raises(dataclasses_FrozenError()):
        f.probe_bitrate_kbps = 999.0  # type: ignore[misc]


def dataclasses_FrozenError():
    """Locate FrozenInstanceError across dataclass module versions."""
    import dataclasses

    return dataclasses.FrozenInstanceError


# --- Predictor analytical fallback ---------------------------------


@pytest.mark.parametrize("codec", list(known_codecs()))
def test_predictor_monotone_in_crf(codec: str):
    """Predictor is non-strictly monotone-decreasing in CRF."""
    p = Predictor()
    feats = ShotFeatures(
        probe_bitrate_kbps=3000.0,
        probe_i_frame_avg_bytes=12000.0,
        probe_p_frame_avg_bytes=2500.0,
        probe_b_frame_avg_bytes=1200.0,
    )
    from vmaftune.codec_adapters import get_adapter

    a = get_adapter(codec)
    lo, hi = a.quality_range
    prev = 200.0
    for crf in range(lo, hi + 1, max(1, (hi - lo) // 8)):
        v = p.predict_vmaf(feats, crf, codec)
        assert 0.0 <= v <= 100.0
        assert v <= prev + 1e-6, f"non-monotone at crf={crf}: {v} > {prev}"
        prev = v


def test_predictor_bitrate_correction_lifts_predicted_vmaf():
    """A high-bitrate (high-complexity-but-encoder-spending-bits) shot
    should predict higher VMAF at the same CRF than a low-bitrate one."""
    p = Predictor()
    low = ShotFeatures(
        probe_bitrate_kbps=200.0,
        probe_i_frame_avg_bytes=0,
        probe_p_frame_avg_bytes=0,
        probe_b_frame_avg_bytes=0,
    )
    high = ShotFeatures(
        probe_bitrate_kbps=20000.0,
        probe_i_frame_avg_bytes=0,
        probe_p_frame_avg_bytes=0,
        probe_b_frame_avg_bytes=0,
    )
    assert p.predict_vmaf(high, 28, "libx264") > p.predict_vmaf(low, 28, "libx264")


def test_predictor_pick_crf_in_range():
    p = Predictor()
    feats = ShotFeatures(
        probe_bitrate_kbps=3000.0,
        probe_i_frame_avg_bytes=10000,
        probe_p_frame_avg_bytes=2000,
        probe_b_frame_avg_bytes=1000,
    )
    crf = p.pick_crf(feats, 90.0, "libx264")
    from vmaftune.codec_adapters import get_adapter

    lo, hi = get_adapter("libx264").quality_range
    assert lo <= crf <= hi


def test_predictor_pick_crf_higher_target_picks_lower_crf():
    """Asking for higher quality picks a lower CRF (more bits)."""
    p = Predictor()
    feats = ShotFeatures(
        probe_bitrate_kbps=3000.0,
        probe_i_frame_avg_bytes=10000,
        probe_p_frame_avg_bytes=2000,
        probe_b_frame_avg_bytes=1000,
    )
    crf_92 = p.pick_crf(feats, 92.0, "libx264")
    crf_96 = p.pick_crf(feats, 96.0, "libx264")
    assert crf_96 <= crf_92


# --- pick_keyint heuristic -----------------------------------------


def test_pick_keyint_long_low_motion_extends_gop():
    feats = ShotFeatures(
        probe_bitrate_kbps=500.0,  # well below low_motion_threshold
        probe_i_frame_avg_bytes=0,
        probe_p_frame_avg_bytes=0,
        probe_b_frame_avg_bytes=0,
        shot_length_frames=240,  # 10 s at 24 fps
    )
    keyint, min_keyint = pick_keyint(feats, 24.0)
    assert keyint == 96  # 4 * fps
    assert min_keyint == 24


def test_pick_keyint_high_motion_tightens_gop():
    feats = ShotFeatures(
        probe_bitrate_kbps=12000.0,  # above high_motion_threshold
        probe_i_frame_avg_bytes=0,
        probe_p_frame_avg_bytes=0,
        probe_b_frame_avg_bytes=0,
        shot_length_frames=120,
    )
    keyint, _ = pick_keyint(feats, 24.0)
    assert keyint == 24  # 1 * fps


def test_pick_keyint_default_band():
    feats = ShotFeatures(
        probe_bitrate_kbps=3000.0,
        probe_i_frame_avg_bytes=0,
        probe_p_frame_avg_bytes=0,
        probe_b_frame_avg_bytes=0,
        shot_length_frames=120,
    )
    keyint, _ = pick_keyint(feats, 24.0)
    assert keyint == 48  # 2 * fps


# --- resolution_class ----------------------------------------------


def test_resolution_class_buckets():
    assert resolution_class(360) == "sd"
    assert resolution_class(720) == "hd_ready"
    assert resolution_class(1080) == "hd"
    assert resolution_class(2160) == "uhd"
    assert resolution_class(4320) == "uhd8k"
    assert resolution_class(9999) == "uhd8k"


# --- select_validation_shots --------------------------------------


def test_select_validation_shots_stratified_pulls_from_each_quartile():
    shots = [Shot(i * 100, (i + 1) * 100) for i in range(40)]
    # Bitrate is monotonically increasing with shot index → quartiles
    # are the four contiguous chunks of 10.
    feats = {
        s: ShotFeatures(
            probe_bitrate_kbps=float(i),
            probe_i_frame_avg_bytes=0,
            probe_p_frame_avg_bytes=0,
            probe_b_frame_avg_bytes=0,
        )
        for i, s in enumerate(shots)
    }
    selected = select_validation_shots(shots, feats, k=8)
    assert len(selected) == 8
    # At least one shot from each quartile
    indices = [shots.index(s) for s in selected]
    assert any(i < 10 for i in indices)
    assert any(10 <= i < 20 for i in indices)
    assert any(20 <= i < 30 for i in indices)
    assert any(i >= 30 for i in indices)


def test_select_validation_shots_handles_short_list():
    shots = [Shot(0, 100), Shot(100, 200)]
    feats = {
        s: ShotFeatures(
            probe_bitrate_kbps=0,
            probe_i_frame_avg_bytes=0,
            probe_p_frame_avg_bytes=0,
            probe_b_frame_avg_bytes=0,
        )
        for s in shots
    }
    selected = select_validation_shots(shots, feats, k=8)
    assert tuple(selected) == tuple(shots)


# --- validate_predictor verdicts -----------------------------------


def _residuals(target: float, deltas: list[float]) -> tuple[ShotResidual, ...]:
    out: list[ShotResidual] = []
    for i, delta in enumerate(deltas):
        out.append(
            ShotResidual(
                shot=Shot(i * 100, (i + 1) * 100),
                crf_picked=23,
                predicted_vmaf=target,
                measured_vmaf=target + delta,
            )
        )
    return tuple(out)


def test_decide_verdict_gospel_when_all_within_threshold():
    rep = _decide_verdict(_residuals(90.0, [0.5, -1.0, 1.0, -0.5]), target_vmaf=90.0, threshold=1.5)
    assert rep.verdict == Verdict.GOSPEL


def test_decide_verdict_recalibrate_when_biased_but_tight():
    # All residuals around +2.0 — biased high, spread tight.
    rep = _decide_verdict(_residuals(90.0, [2.0, 2.5, 1.8, 2.2]), target_vmaf=90.0, threshold=1.5)
    assert rep.verdict == Verdict.RECALIBRATE
    assert rep.bias_correction == pytest.approx(2.125, abs=0.01)


def test_decide_verdict_fall_back_when_spread_wide():
    # Spread spans 6 VMAF — nothing the bias shift can save.
    rep = _decide_verdict(_residuals(90.0, [3.0, -3.0, 0.5, -2.5]), target_vmaf=90.0, threshold=1.5)
    assert rep.verdict == Verdict.FALL_BACK


def test_decide_verdict_fall_back_on_empty_residuals():
    rep = _decide_verdict((), target_vmaf=90.0, threshold=1.5)
    assert rep.verdict == Verdict.FALL_BACK


def test_validate_predictor_end_to_end_with_stubs():
    shots = [Shot(i * 100, (i + 1) * 100) for i in range(8)]
    p = Predictor()

    def feat(s: Shot) -> ShotFeatures:
        return ShotFeatures(
            probe_bitrate_kbps=float(s.start_frame),
            probe_i_frame_avg_bytes=0,
            probe_p_frame_avg_bytes=0,
            probe_b_frame_avg_bytes=0,
        )

    def real(s: Shot, crf: int, codec: str) -> tuple[Path, float]:
        # Pretend the encoder lands exactly on the predictor's score.
        return Path("/dev/null"), p.predict_vmaf(feat(s), crf, codec)

    rep = validate_predictor(
        predictor=p,
        shots=shots,
        target_vmaf=92.0,
        codec="libx264",
        feature_extractor=feat,
        real_encode_and_score=real,
        k=4,
    )
    assert rep.verdict == Verdict.GOSPEL
    assert len(rep.residuals) == 4


# --- make_predictor_predicate adapter ----------------------------


def test_make_predictor_predicate_returns_pickable_pair():
    p = Predictor()

    def feat(_shot, _enc):
        return ShotFeatures(
            probe_bitrate_kbps=3000.0,
            probe_i_frame_avg_bytes=0,
            probe_p_frame_avg_bytes=0,
            probe_b_frame_avg_bytes=0,
        )

    pred = make_predictor_predicate(p, feat)
    crf, vmaf = pred(Shot(0, 100), 92.0, "libx264")
    assert isinstance(crf, int)
    assert 15 <= crf <= 40
    assert 0.0 <= vmaf <= 100.0


# --- predictor_features parsers ----------------------------------


def test_parse_bitrate_picks_last_value():
    stderr = (
        "frame=  10 fps=...  bitrate=2500.5kbits/s\n"
        "frame=  20 fps=...  bitrate=2300.0kbits/s\n"
        "size=...  bitrate=2150.7kbits/s\n"
    )
    assert _parse_bitrate(stderr) == pytest.approx(2150.7)


def test_parse_bitrate_returns_zero_on_no_match():
    assert _parse_bitrate("nothing relevant here") == 0.0


def test_parse_metadata_float_handles_trailing_whitespace():
    assert _parse_metadata_float("lavfi.signalstats.YAVG= 128.42 ") == pytest.approx(128.42)


def test_parse_signalstats_averages_per_metric():
    text = (
        "lavfi.signalstats.YAVG=120.0\n"
        "lavfi.signalstats.YAVG=130.0\n"
        "lavfi.signalstats.YDIF=2.0\n"
        "lavfi.signalstats.YDIF=4.0\n"
    )
    s = _parse_signalstats(text)
    assert s.y_avg == pytest.approx(125.0)
    assert s.frame_diff_mean == pytest.approx(3.0)


def test_compute_saliency_extracts_raw_yuv_before_model(tmp_path, monkeypatch):
    np = pytest.importorskip("numpy")
    import vmaftune.saliency as saliency

    source = tmp_path / "source.mp4"
    source.write_bytes(b"fake container")
    model = tmp_path / "saliency.onnx"
    model.write_bytes(b"fake model")
    seen: dict[str, object] = {}

    def fake_run(cmd, capture_output, text, check):
        assert cmd[0] == "ffmpeg-test"
        assert "-ss" in cmd and cmd[cmd.index("-ss") + 1] == "1.000000"
        assert "-frames:v" in cmd and cmd[cmd.index("-frames:v") + 1] == "10"
        raw_path = Path(cmd[-1])
        raw_path.write_bytes(bytes(4 * 4 * 3 // 2 * 10))
        return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")

    def fake_compute(video_path, width, height, *, model_path, frame_samples):
        seen["video_path"] = Path(video_path)
        seen["width"] = width
        seen["height"] = height
        seen["model_path"] = model_path
        seen["frame_samples"] = frame_samples
        assert Path(video_path).is_file()
        return np.asarray([[0.0, 0.5], [1.0, 0.5]], dtype=np.float32)

    monkeypatch.setattr(saliency, "compute_saliency_map", fake_compute)
    cfg = FeatureExtractorConfig(
        ffmpeg_bin="ffmpeg-test",
        use_saliency=True,
        saliency_model=model,
        saliency_frame_samples=3,
        probe_max_frames=10,
    )

    mean, var = _compute_saliency(
        Shot(24, 72),
        source,
        cfg,
        fake_run,
        width=4,
        height=4,
        fps=24.0,
    )

    assert mean == pytest.approx(0.5)
    assert var == pytest.approx(0.125)
    assert seen["width"] == 4
    assert seen["height"] == 4
    assert seen["model_path"] == model
    assert seen["frame_samples"] == 3
