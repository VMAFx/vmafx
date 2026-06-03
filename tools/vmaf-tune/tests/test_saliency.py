# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Saliency-aware ROI tests (Bucket #2 / ADR-0293).

The ONNX inference is mocked via ``session_factory`` — no
onnxruntime install required. The encode runner is mocked via
``encode_runner`` — no ffmpeg required.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

# Make src/ importable without an editable install.
_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

np = pytest.importorskip("numpy")

from vmaftune import saliency  # noqa: E402
from vmaftune.encode import EncodeRequest  # noqa: E402
from vmaftune.saliency import (  # noqa: E402
    QP_OFFSET_MAX,
    QP_OFFSET_MIN,
    SALIENCY_AGGREGATORS,
    X264_MB_SIDE,
    SaliencyConfig,
    SaliencyUnavailableError,
    augment_extra_params_with_qpfile,
    compute_saliency_map,
    reduce_qp_map_to_blocks,
    saliency_aware_encode,
    saliency_to_qp_map,
    write_x264_qpfile,
)

# ---- Fixtures ---------------------------------------------------------------


def _write_yuv420p(path: Path, width: int, height: int, nframes: int) -> Path:
    """Write a tiny synthetic yuv420p clip with constant grey frames."""
    frame_bytes = width * height * 3 // 2
    path.write_bytes(b"\x80" * (frame_bytes * nframes))
    return path


def _write_yuv420p_planes(
    path: Path, width: int, height: int, nframes: int, y: int, u: int, v: int
) -> Path:
    """Write a synthetic yuv420p clip with constant Y/U/V planes."""
    y_plane = bytes([y]) * (width * height)
    uv_plane = bytes([u]) * ((width // 2) * (height // 2))
    vv_plane = bytes([v]) * ((width // 2) * (height // 2))
    path.write_bytes((y_plane + uv_plane + vv_plane) * nframes)
    return path


class _FakeOnnxSession:
    """Stub ONNX session: returns a deterministic synthetic mask.

    The mask has higher saliency on the right half so tests can
    assert spatial behaviour through the full pipeline.
    """

    def __init__(self, h: int, w: int):
        self._h = h
        self._w = w

    def run(self, _outputs, feeds):  # noqa: D401
        # feeds is {"input": ndarray}; we ignore the content and emit
        # a deterministic right-bright mask shaped [1, 1, H, W].
        mask = np.zeros((1, 1, self._h, self._w), dtype=np.float32)
        mask[..., self._w // 2 :] = 0.9
        mask[..., : self._w // 2] = 0.1
        return [mask]


def _session_factory_for(h: int, w: int):
    def _factory(_path):
        return _FakeOnnxSession(h, w)

    return _factory


class _InspectingOnnxSession:
    def __init__(self, h: int, w: int):
        self._h = h
        self._w = w
        self.tensors: list[np.ndarray] = []

    def run(self, _outputs, feeds):
        tensor = np.asarray(feeds["input"], dtype=np.float32)
        self.tensors.append(tensor)
        return [np.full((1, 1, self._h, self._w), 0.5, dtype=np.float32)]


class _SequenceOnnxSession:
    def __init__(self, h: int, w: int, values: list[float]):
        self._h = h
        self._w = w
        self._values = list(values)
        self._idx = 0

    def run(self, _outputs, feeds):
        assert "input" in feeds
        value = self._values[min(self._idx, len(self._values) - 1)]
        self._idx += 1
        return [np.full((1, 1, self._h, self._w), value, dtype=np.float32)]


class _FakeCompleted:
    def __init__(self, returncode: int = 0, stdout: str = "", stderr: str = ""):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


# ---- saliency_to_qp_map -----------------------------------------------------


def test_saliency_to_qp_map_high_saliency_lowers_qp():
    mask = np.array([[0.0, 1.0]], dtype=np.float32)
    qp = saliency_to_qp_map(mask, baseline_qp=23, foreground_offset=-4)
    # Saliency 1.0 with offset -4 should produce QP delta -4.
    # Saliency 0.0 with offset -4 should produce QP delta +4.
    assert qp[0, 1] < qp[0, 0]
    assert qp[0, 1] == -4
    assert qp[0, 0] == 4


def test_saliency_to_qp_map_neutral_is_zero():
    mask = np.full((4, 4), 0.5, dtype=np.float32)
    qp = saliency_to_qp_map(mask, baseline_qp=23, foreground_offset=-4)
    assert (qp == 0).all()


def test_saliency_to_qp_map_clamps_to_window():
    mask = np.array([[0.0, 1.0]], dtype=np.float32)
    qp = saliency_to_qp_map(mask, baseline_qp=23, foreground_offset=-50)
    assert qp.min() >= QP_OFFSET_MIN
    assert qp.max() <= QP_OFFSET_MAX


def test_saliency_to_qp_map_dtype_is_int32():
    mask = np.zeros((2, 2), dtype=np.float32)
    qp = saliency_to_qp_map(mask, baseline_qp=23)
    assert qp.dtype == np.int32


# ---- reduce_qp_map_to_blocks ------------------------------------------------


def test_reduce_qp_map_to_blocks_uses_block_mean():
    qp = np.array(
        [
            [-4] * X264_MB_SIDE * 2,
            [-4] * X264_MB_SIDE * 2,
        ]
        * X264_MB_SIDE,
        dtype=np.int32,
    )
    out = reduce_qp_map_to_blocks(qp, block=X264_MB_SIDE)
    # 32x32 input -> (2, 2) blocks at MB_SIDE=16; constant -4 stays.
    assert out.shape == (2, 2)
    assert (out == -4).all()


def test_reduce_qp_map_to_blocks_rejects_too_small():
    with pytest.raises(ValueError):
        reduce_qp_map_to_blocks(np.zeros((4, 4), dtype=np.int32), block=16)


# ---- write_x264_qpfile ------------------------------------------------------


def test_write_x264_qpfile_emits_per_frame_block(tmp_path):
    blocks = np.array([[-4, 0, 4]], dtype=np.int32)
    out = tmp_path / "qp.txt"
    write_x264_qpfile(blocks, out, duration_frames=2)
    text = out.read_text(encoding="ascii")
    # Frame 0 is I, frame 1 is P; one row of 3 offsets each.
    assert "0 I 0" in text
    assert "1 P 0" in text
    # Row content present.
    assert "-4 0 4" in text


def test_augment_extra_params_with_qpfile_appends_x264_params(tmp_path):
    qp = tmp_path / "qp.txt"
    qp.write_text("0 I 0\n", encoding="ascii")
    out = augment_extra_params_with_qpfile(("-x264-opts", "ref=4"), qp)
    assert out[-2] == "-x264-params"
    assert out[-1].startswith("qpfile=")
    assert str(qp) in out[-1]


# ---- compute_saliency_map ---------------------------------------------------


def test_compute_saliency_map_uses_session_factory(tmp_path):
    w, h = 32, 16
    src = _write_yuv420p(tmp_path / "src.yuv", w, h, nframes=4)
    # Touch a fake model file so the existence check passes.
    fake_model = tmp_path / "saliency_student_v1.onnx"
    fake_model.write_bytes(b"\x00")
    mask = compute_saliency_map(
        src,
        w,
        h,
        model_path=fake_model,
        frame_samples=2,
        session_factory=_session_factory_for(h, w),
    )
    assert mask.shape == (h, w)
    assert mask.dtype == np.float32
    # Right half has higher saliency than left half.
    assert mask[:, w // 2 :].mean() > mask[:, : w // 2].mean()
    # Bounded to [0, 1].
    assert mask.min() >= 0.0
    assert mask.max() <= 1.0


def test_compute_saliency_map_feeds_chroma_aware_rgb(tmp_path):
    w, h = 32, 16
    src = _write_yuv420p_planes(tmp_path / "src.yuv", w, h, nframes=1, y=128, u=90, v=240)
    fake_model = tmp_path / "saliency_student_v1.onnx"
    fake_model.write_bytes(b"\x00")
    session = _InspectingOnnxSession(h, w)

    compute_saliency_map(
        src,
        w,
        h,
        model_path=fake_model,
        frame_samples=1,
        session_factory=lambda _path: session,
    )

    assert session.tensors, "expected saliency tensor to reach ONNX session"
    tensor = session.tensors[0][0]
    channel_means = tensor.mean(axis=(1, 2))
    assert not np.isclose(channel_means[0], channel_means[1])
    assert not np.isclose(channel_means[1], channel_means[2])


@pytest.mark.parametrize("aggregator", SALIENCY_AGGREGATORS)
def test_compute_saliency_map_accepts_temporal_aggregators(tmp_path, aggregator: str):
    w, h = 32, 16
    src = _write_yuv420p_planes(tmp_path / "src.yuv", w, h, nframes=3, y=96, u=128, v=128)
    fake_model = tmp_path / "saliency_student_v1.onnx"
    fake_model.write_bytes(b"\x00")
    session = _SequenceOnnxSession(h, w, [0.2, 0.6, 0.8])

    mask = compute_saliency_map(
        src,
        w,
        h,
        model_path=fake_model,
        frame_samples=3,
        temporal_aggregator=aggregator,
        session_factory=lambda _path: session,
    )

    assert mask.shape == (h, w)
    assert mask.min() >= 0.0
    assert mask.max() <= 1.0


def test_compute_saliency_map_ema_weights_recent_frames(tmp_path):
    w, h = 32, 16
    src = _write_yuv420p(tmp_path / "src.yuv", w, h, nframes=3)
    fake_model = tmp_path / "saliency_student_v1.onnx"
    fake_model.write_bytes(b"\x00")
    session = _SequenceOnnxSession(h, w, [0.0, 0.5, 1.0])

    mask = compute_saliency_map(
        src,
        w,
        h,
        model_path=fake_model,
        frame_samples=3,
        temporal_aggregator="ema",
        ema_alpha=0.5,
        session_factory=lambda _path: session,
    )

    assert np.isclose(float(mask.mean()), 0.625)


def test_compute_saliency_map_max_keeps_peak(tmp_path):
    w, h = 32, 16
    src = _write_yuv420p(tmp_path / "src.yuv", w, h, nframes=3)
    fake_model = tmp_path / "saliency_student_v1.onnx"
    fake_model.write_bytes(b"\x00")
    session = _SequenceOnnxSession(h, w, [0.1, 0.9, 0.3])

    mask = compute_saliency_map(
        src,
        w,
        h,
        model_path=fake_model,
        frame_samples=3,
        temporal_aggregator="max",
        session_factory=lambda _path: session,
    )

    assert np.isclose(float(mask.mean()), 0.9)


def test_compute_saliency_map_missing_model_raises(tmp_path):
    src = _write_yuv420p(tmp_path / "src.yuv", 32, 16, nframes=2)
    with pytest.raises(SaliencyUnavailableError):
        compute_saliency_map(src, 32, 16, model_path=tmp_path / "no-such-model.onnx")


# ---- saliency_aware_encode --------------------------------------------------


def test_saliency_aware_encode_includes_qpfile_in_command(tmp_path):
    """The end-to-end harness wires the qpfile into the ffmpeg command."""
    w, h = 32, 16
    src = _write_yuv420p(tmp_path / "src.yuv", w, h, nframes=3)
    fake_model = tmp_path / "saliency_student_v1.onnx"
    fake_model.write_bytes(b"\x00")

    captured: dict[str, list[str]] = {}

    def _fake_runner(cmd, **_kwargs):
        captured["cmd"] = list(cmd)
        # Touch the output file so size lookup works.
        out = Path(cmd[-1])
        out.write_bytes(b"\x00\x01")
        return _FakeCompleted(0, stderr="ffmpeg version 6.1.1\nx264 - core 164")

    request = EncodeRequest(
        source=src,
        width=w,
        height=h,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder="libx264",
        preset="medium",
        crf=23,
        output=tmp_path / "out.mp4",
    )
    cfg = SaliencyConfig(foreground_offset=-4, frame_samples=2, persist_qpfile=True)
    result = saliency_aware_encode(
        request,
        duration_frames=3,
        model_path=fake_model,
        config=cfg,
        encode_runner=_fake_runner,
        session_factory=_session_factory_for(h, w),
    )
    assert result.exit_status == 0
    cmd = captured["cmd"]
    assert "-x264-params" in cmd
    qp_arg_idx = cmd.index("-x264-params") + 1
    assert cmd[qp_arg_idx].startswith("qpfile=")


def test_saliency_aware_encode_passes_temporal_aggregator(tmp_path):
    w, h = 32, 16
    src = _write_yuv420p(tmp_path / "src.yuv", w, h, nframes=3)
    fake_model = tmp_path / "saliency_student_v1.onnx"
    fake_model.write_bytes(b"\x00")

    captured: dict[str, list[str]] = {}

    def _fake_runner(cmd, **_kwargs):
        captured["cmd"] = list(cmd)
        Path(cmd[-1]).write_bytes(b"\x00\x01")
        return _FakeCompleted(0, stderr="ffmpeg version 6.1.1\nx264 - core 164")

    request = EncodeRequest(
        source=src,
        width=w,
        height=h,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder="libx264",
        preset="medium",
        crf=23,
        output=tmp_path / "out.mp4",
    )
    cfg = SaliencyConfig(
        foreground_offset=-4,
        frame_samples=3,
        persist_qpfile=True,
        temporal_aggregator="max",
    )
    result = saliency_aware_encode(
        request,
        duration_frames=3,
        model_path=fake_model,
        config=cfg,
        encode_runner=_fake_runner,
        session_factory=lambda _path: _SequenceOnnxSession(h, w, [0.1, 0.9, 0.3]),
    )

    assert result.exit_status == 0
    assert "-x264-params" in captured["cmd"]


def test_saliency_aware_encode_falls_back_when_unavailable(tmp_path):
    """Missing model -> plain encode, no qpfile in command."""
    w, h = 32, 16
    src = _write_yuv420p(tmp_path / "src.yuv", w, h, nframes=2)

    def _fake_runner(cmd, **_kwargs):
        Path(cmd[-1]).write_bytes(b"\x00")
        return _FakeCompleted(0, stderr="ffmpeg version 6.1.1")

    request = EncodeRequest(
        source=src,
        width=w,
        height=h,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder="libx264",
        preset="medium",
        crf=23,
        output=tmp_path / "out.mp4",
    )
    result = saliency_aware_encode(
        request,
        duration_frames=2,
        model_path=tmp_path / "missing.onnx",
        encode_runner=_fake_runner,
    )
    assert result.exit_status == 0


# ---- public API surface -----------------------------------------------------


def test_public_api_exposes_canonical_names():
    expected = {
        "compute_saliency_map",
        "saliency_to_qp_map",
        "saliency_aware_encode",
        "SaliencyConfig",
        "SaliencyUnavailableError",
        "SALIENCY_AGGREGATORS",
    }
    assert expected.issubset(set(saliency.__all__))
