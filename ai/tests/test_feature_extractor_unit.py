# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for :mod:`ai.data.feature_extractor` — uncovered branches.

Covers:
- ``_ensure_binary`` happy + missing-binary paths
- ``_lookup`` key present / ``integer_`` fallback / missing
- ``_run_vmaf_json`` argv composition and temp-file cleanup on error
- ``extract_features`` happy path, NaN-fill for missing metrics, empty frames
- ``aggregate_clip_stats`` mean/p10/p90/std computation, NaN handling,
  empty-frame fallback, bad-ndim error path
"""

from __future__ import annotations

import json
import subprocess
from pathlib import Path
from unittest.mock import patch

import numpy as np
import pytest

import ai.data.feature_extractor as fe_mod
from ai.data.feature_extractor import (
    DEFAULT_FEATURES,
    FeatureExtractionResult,
    _ensure_binary,
    _lookup,
    _run_vmaf_json,
    aggregate_clip_stats,
    extract_features,
)

# ---------------------------------------------------------------------------
# _ensure_binary
# ---------------------------------------------------------------------------


def test_ensure_binary_raises_when_missing(tmp_path: Path) -> None:
    absent = tmp_path / "no-such-vmaf"
    with pytest.raises(RuntimeError, match="libvmaf CLI not found"):
        _ensure_binary(absent)


def test_ensure_binary_passes_when_file_exists(tmp_path: Path) -> None:
    binary = tmp_path / "vmaf"
    binary.write_text("")
    # Should not raise.
    _ensure_binary(binary)


# ---------------------------------------------------------------------------
# _lookup
# ---------------------------------------------------------------------------


def test_lookup_direct_key() -> None:
    metrics = {"adm2": 0.95, "vif_scale0": 0.8}
    assert _lookup(metrics, "adm2") == pytest.approx(0.95)


def test_lookup_integer_fallback() -> None:
    metrics = {"integer_adm2": 0.92}
    assert _lookup(metrics, "adm2") == pytest.approx(0.92)


def test_lookup_returns_none_when_absent() -> None:
    metrics = {"vif_scale0": 0.7}
    assert _lookup(metrics, "psnr_y") is None


def test_lookup_prefers_direct_over_integer_prefix() -> None:
    metrics = {"adm2": 0.95, "integer_adm2": 0.80}
    assert _lookup(metrics, "adm2") == pytest.approx(0.95)


# ---------------------------------------------------------------------------
# _run_vmaf_json — argv composition and cleanup
# ---------------------------------------------------------------------------


def _make_fake_run_ok(out_payload: dict):
    """Return a side-effect function for subprocess.run that writes the output JSON."""

    def fake_run(cmd, **kw):
        out_path = Path(cmd[cmd.index("-o") + 1])
        out_path.write_text(json.dumps(out_payload))
        return subprocess.CompletedProcess(cmd, returncode=0, stdout="", stderr="")

    return fake_run


def test_run_vmaf_json_composes_argv(tmp_path: Path) -> None:
    binary = tmp_path / "vmaf"
    binary.write_text("")
    ref = tmp_path / "ref.yuv"
    dis = tmp_path / "dis.yuv"
    ref.write_bytes(b"\x00" * 16)
    dis.write_bytes(b"\x00" * 16)

    payload = {"frames": []}
    captured: list[list[str]] = []

    def fake_run(cmd, **kw):
        captured.append(list(cmd))
        out_path = Path(cmd[cmd.index("-o") + 1])
        out_path.write_text(json.dumps(payload))
        return subprocess.CompletedProcess(cmd, returncode=0, stdout="", stderr="")

    with patch.object(fe_mod.subprocess, "run", side_effect=fake_run):
        result = _run_vmaf_json(binary, ref, dis, 320, 240, features=DEFAULT_FEATURES)

    assert captured, "subprocess.run not called"
    argv = captured[0]
    assert argv[0] == str(binary)
    assert argv[argv.index("-r") + 1] == str(ref)
    assert argv[argv.index("-d") + 1] == str(dis)
    assert argv[argv.index("-w") + 1] == "320"
    assert argv[argv.index("-h") + 1] == "240"
    assert argv[argv.index("-p") + 1] == "420"
    assert argv[argv.index("-b") + 1] == "8"
    assert "--json" in argv
    assert result == payload


def test_run_vmaf_json_passes_custom_pix_fmt_and_bitdepth(tmp_path: Path) -> None:
    binary = tmp_path / "vmaf"
    binary.write_text("")

    def fake_run(cmd, **kw):
        out_path = Path(cmd[cmd.index("-o") + 1])
        out_path.write_text(json.dumps({"frames": []}))
        return subprocess.CompletedProcess(cmd, returncode=0)

    with patch.object(fe_mod.subprocess, "run", side_effect=fake_run) as m:
        _run_vmaf_json(
            binary,
            tmp_path / "r.yuv",
            tmp_path / "d.yuv",
            1920,
            1080,
            pix_fmt="444",
            bitdepth=10,
            features=DEFAULT_FEATURES,
        )
    argv = m.call_args[0][0]
    assert argv[argv.index("-p") + 1] == "444"
    assert argv[argv.index("-b") + 1] == "10"


def test_run_vmaf_json_includes_feature_flags(tmp_path: Path) -> None:
    binary = tmp_path / "vmaf"
    binary.write_text("")

    def fake_run(cmd, **kw):
        out_path = Path(cmd[cmd.index("-o") + 1])
        out_path.write_text(json.dumps({"frames": []}))
        return subprocess.CompletedProcess(cmd, returncode=0)

    with patch.object(fe_mod.subprocess, "run", side_effect=fake_run) as m:
        _run_vmaf_json(
            binary,
            tmp_path / "r.yuv",
            tmp_path / "d.yuv",
            320,
            240,
            features=("adm2", "vif_scale0"),
        )
    argv = m.call_args[0][0]
    feature_flags = [argv[i + 1] for i, a in enumerate(argv) if a == "--feature"]
    # adm2 → extractor "adm", vif_scale0 → extractor "vif"
    assert "adm" in feature_flags
    assert "vif" in feature_flags


def test_run_vmaf_json_cleans_up_temp_on_subprocess_error(tmp_path: Path) -> None:
    binary = tmp_path / "vmaf"
    binary.write_text("")
    leftover: list[Path] = []

    def fake_run(cmd, **kw):
        leftover.append(Path(cmd[cmd.index("-o") + 1]))
        raise subprocess.CalledProcessError(returncode=1, cmd=cmd)

    with (
        patch.object(fe_mod.subprocess, "run", side_effect=fake_run),
        pytest.raises(subprocess.CalledProcessError),
    ):
        _run_vmaf_json(
            binary,
            tmp_path / "r.yuv",
            tmp_path / "d.yuv",
            16,
            16,
            features=DEFAULT_FEATURES,
        )

    assert leftover and not leftover[0].exists(), "Temp output file was not removed"


# ---------------------------------------------------------------------------
# extract_features — end-to-end with mocked subprocess
# ---------------------------------------------------------------------------


def _stub_frame(frame_num: int, metrics: dict) -> dict:
    return {"frameNum": frame_num, "metrics": metrics}


def test_extract_features_happy_path(tmp_path: Path) -> None:
    binary = tmp_path / "vmaf"
    binary.write_text("")
    ref = tmp_path / "ref.yuv"
    dis = tmp_path / "dis.yuv"
    ref.write_bytes(b"\x00")
    dis.write_bytes(b"\x00")

    doc = {
        "frames": [
            _stub_frame(
                0,
                {
                    "adm2": 0.9,
                    "vif_scale0": 0.8,
                    "vif_scale1": 0.7,
                    "vif_scale2": 0.6,
                    "vif_scale3": 0.5,
                    "motion2": 0.3,
                },
            ),
            _stub_frame(
                1,
                {
                    "adm2": 0.85,
                    "vif_scale0": 0.75,
                    "vif_scale1": 0.65,
                    "vif_scale2": 0.55,
                    "vif_scale3": 0.45,
                    "motion2": 0.25,
                },
            ),
        ]
    }

    with (
        patch.object(fe_mod, "_ensure_binary"),
        patch.object(fe_mod, "_run_vmaf_json", return_value=doc),
    ):
        result = extract_features(ref, dis, 320, 240, vmaf_binary=binary)

    assert isinstance(result, FeatureExtractionResult)
    assert result.n_frames == 2
    assert result.per_frame.shape == (2, len(DEFAULT_FEATURES))
    assert result.per_frame.dtype == np.float32
    assert result.feature_names == DEFAULT_FEATURES
    # Check one value.
    adm2_idx = list(DEFAULT_FEATURES).index("adm2")
    assert result.per_frame[0, adm2_idx] == pytest.approx(0.9)


def test_extract_features_nan_for_missing_metric(tmp_path: Path) -> None:
    binary = tmp_path / "vmaf"
    binary.write_text("")

    doc = {
        "frames": [
            _stub_frame(0, {"adm2": 0.9}),  # only adm2, others absent
        ]
    }

    with (
        patch.object(fe_mod, "_ensure_binary"),
        patch.object(fe_mod, "_run_vmaf_json", return_value=doc),
    ):
        result = extract_features(
            binary,
            binary,
            16,
            16,
            features=("adm2", "vif_scale0"),
            vmaf_binary=binary,
        )

    adm2_idx = 0
    vif_idx = 1
    assert result.per_frame[0, adm2_idx] == pytest.approx(0.9)
    assert np.isnan(result.per_frame[0, vif_idx])


def test_extract_features_integer_fallback(tmp_path: Path) -> None:
    binary = tmp_path / "vmaf"
    binary.write_text("")

    doc = {
        "frames": [
            _stub_frame(0, {"integer_adm2": 0.88}),  # integer_ prefix variant
        ]
    }

    with (
        patch.object(fe_mod, "_ensure_binary"),
        patch.object(fe_mod, "_run_vmaf_json", return_value=doc),
    ):
        result = extract_features(binary, binary, 16, 16, features=("adm2",), vmaf_binary=binary)

    assert result.per_frame[0, 0] == pytest.approx(0.88)


def test_extract_features_empty_frames_returns_zero_shape(tmp_path: Path) -> None:
    binary = tmp_path / "vmaf"
    binary.write_text("")

    doc = {"frames": []}

    with (
        patch.object(fe_mod, "_ensure_binary"),
        patch.object(fe_mod, "_run_vmaf_json", return_value=doc),
    ):
        result = extract_features(
            binary, binary, 16, 16, features=("adm2", "motion2"), vmaf_binary=binary
        )

    assert result.n_frames == 0
    assert result.per_frame.shape == (0, 2)


def test_extract_features_raises_when_binary_absent(tmp_path: Path) -> None:
    absent = tmp_path / "no-vmaf"
    with pytest.raises(RuntimeError, match="libvmaf CLI not found"):
        extract_features(tmp_path / "r.yuv", tmp_path / "d.yuv", 16, 16, vmaf_binary=absent)


def test_extract_features_uses_default_binary_from_env(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """VMAF_BIN env var is picked up by default_vmaf_binary()."""
    fake_binary = tmp_path / "my-vmaf"
    fake_binary.write_text("")
    monkeypatch.setenv("VMAF_BIN", str(fake_binary))

    doc = {"frames": []}

    with (
        patch.object(fe_mod, "_ensure_binary"),
        patch.object(fe_mod, "_run_vmaf_json", return_value=doc),
    ):
        result = extract_features(tmp_path / "r.yuv", tmp_path / "d.yuv", 16, 16)

    assert result.n_frames == 0


# ---------------------------------------------------------------------------
# FeatureExtractionResult serialisation round-trip
# ---------------------------------------------------------------------------


def test_feature_extraction_result_round_trip() -> None:
    arr = np.array([[0.1, 0.2, 0.3], [0.4, 0.5, 0.6]], dtype=np.float32)
    result = FeatureExtractionResult(
        feature_names=("adm2", "vif_scale0", "motion2"),
        per_frame=arr,
        n_frames=2,
    )
    payload = result.to_jsonable()
    restored = FeatureExtractionResult.from_jsonable(payload)

    assert restored.feature_names == ("adm2", "vif_scale0", "motion2")
    assert restored.n_frames == 2
    assert restored.per_frame.dtype == np.float32
    np.testing.assert_allclose(restored.per_frame, arr)


# ---------------------------------------------------------------------------
# aggregate_clip_stats
# ---------------------------------------------------------------------------


def test_aggregate_clip_stats_shape_is_4n() -> None:
    arr = np.array([[1.0, 2.0], [3.0, 4.0], [5.0, 6.0]], dtype=np.float32)
    out = aggregate_clip_stats(arr)
    assert out.shape == (8,)  # 4 * 2 features


def test_aggregate_clip_stats_mean_is_correct() -> None:
    arr = np.array([[0.0, 10.0], [2.0, 20.0], [4.0, 30.0]], dtype=np.float32)
    out = aggregate_clip_stats(arr)
    n_feat = 2
    mean_block = out[:n_feat]
    np.testing.assert_allclose(mean_block, [2.0, 20.0], rtol=1e-5)


def test_aggregate_clip_stats_p10_p90_ordering() -> None:
    arr = np.arange(100, dtype=np.float32).reshape(100, 1)
    out = aggregate_clip_stats(arr)
    p10 = out[1]
    p90 = out[2]
    assert p10 < p90


def test_aggregate_clip_stats_ignores_nan() -> None:
    arr = np.array([[1.0], [float("nan")], [3.0]], dtype=np.float32)
    out = aggregate_clip_stats(arr)
    # mean of [1, 3] = 2.0 (NaN excluded).
    np.testing.assert_allclose(out[0], 2.0, rtol=1e-5)


def test_aggregate_clip_stats_all_nan_propagates() -> None:
    arr = np.full((3, 1), float("nan"), dtype=np.float32)
    out = aggregate_clip_stats(arr)
    assert np.isnan(out[0])


def test_aggregate_clip_stats_empty_frames_returns_nan_vector() -> None:
    arr = np.zeros((0, 3), dtype=np.float32)
    out = aggregate_clip_stats(arr)
    assert out.shape == (12,)
    assert np.all(np.isnan(out))


def test_aggregate_clip_stats_raises_on_1d_input() -> None:
    arr = np.array([1.0, 2.0, 3.0], dtype=np.float32)
    with pytest.raises(ValueError, match="expected 2-D features"):
        aggregate_clip_stats(arr)


def test_aggregate_clip_stats_output_is_float32() -> None:
    arr = np.ones((5, 4), dtype=np.float64)
    out = aggregate_clip_stats(arr)
    assert out.dtype == np.float32


def test_aggregate_clip_stats_single_frame() -> None:
    # Output layout: [mean_f0, mean_f1, p10_f0, p10_f1, p90_f0, p90_f1, std_f0, std_f1]
    arr = np.array([[50.0, 80.0]], dtype=np.float32)
    out = aggregate_clip_stats(arr)
    # mean of a single frame is that value itself.
    np.testing.assert_allclose(out[0], 50.0, rtol=1e-5)  # mean_f0
    np.testing.assert_allclose(out[1], 80.0, rtol=1e-5)  # mean_f1
    # p10 and p90 of a single value both equal the value.
    np.testing.assert_allclose(out[2], 50.0, rtol=1e-5)  # p10_f0
    np.testing.assert_allclose(out[4], 50.0, rtol=1e-5)  # p90_f0
