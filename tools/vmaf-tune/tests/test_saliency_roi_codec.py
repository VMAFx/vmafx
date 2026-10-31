# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""End-to-end dispatch tests for saliency-aware ROI encoding.

Tests for supported encoders (x265 / SVT-AV1 / libaom) and refused libvvenc verify that
``saliency_aware_encode`` produces the correct per-codec argv when called
with each encoder.  ONNX inference and the encode runner are mocked so no
ffmpeg or onnxruntime install is required.

Companion to:
- ``test_saliency.py`` — x264 end-to-end path + pipeline unit tests.
- ``test_saliency_roi_adapters.py`` — formatter unit tests.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

np = pytest.importorskip("numpy")

from vmaftune.encode import EncodeRequest
from vmaftune.saliency import (
    _SALIENCY_DISPATCH,
    SaliencyConfig,
    SaliencyUnsupportedEncoderError,
    saliency_aware_encode,
)

# ---------------------------------------------------------------------------
# Shared helpers
# ---------------------------------------------------------------------------


def _write_yuv420p(path: Path, width: int, height: int, nframes: int) -> Path:
    """Write a tiny synthetic yuv420p clip with constant grey frames."""
    frame_bytes = width * height * 3 // 2
    path.write_bytes(b"\x80" * (frame_bytes * nframes))
    return path


class _FakeOnnxSession:
    """Stub ONNX session: returns a constant high-saliency mask."""

    def __init__(self, h: int, w: int) -> None:
        self._h = h
        self._w = w

    def run(self, _outputs: object, _feeds: object) -> list[object]:
        mask = np.full((1, 1, self._h, self._w), 0.8, dtype=np.float32)
        return [mask]


def _session_factory_for(h: int, w: int):
    def _factory(_path: object) -> _FakeOnnxSession:
        return _FakeOnnxSession(h, w)

    return _factory


def _make_request(
    tmp_path: Path,
    encoder: str,
    width: int = 128,
    height: int = 128,
    nframes: int = 4,
) -> tuple[EncodeRequest, Path]:
    """Return an EncodeRequest and the written source YUV."""
    src = _write_yuv420p(tmp_path / "src.yuv", width, height, nframes)
    request = EncodeRequest(
        source=src,
        width=width,
        height=height,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder=encoder,
        preset="medium",
        crf=28,
        output=tmp_path / "out.mp4",
    )
    return request, src


def _capture_runner(tmp_path: Path) -> tuple[dict, object]:
    """Return a (capture-dict, runner-callable) pair.

    The runner records the ffmpeg argv and touches the output file so the
    size-lookup in ``run_encode`` succeeds.
    """
    captured: dict[str, list[str]] = {}

    def _runner(cmd: list[str], **_kwargs: object) -> object:
        class _Done:
            returncode = 0
            stdout = ""
            stderr = "ffmpeg version 6.1.1\nlibx265 HEVC encoder"

        # The encoder-version probe (`ffmpeg -version`, cached per process) goes
        # through the same runner: it is not the encode and writes no output.
        if list(cmd[1:]) == ["-version"]:
            return _Done()
        captured["cmd"] = list(cmd)
        Path(cmd[-1]).write_bytes(b"\x00\x01")
        return _Done()

    return captured, _runner


# ---------------------------------------------------------------------------
# Dispatch-table coverage
# ---------------------------------------------------------------------------


def test_saliency_dispatch_table_contains_roi_targets():
    """_SALIENCY_DISPATCH must wire every saliency ROI target."""
    for enc in ("libx264", "libaom-av1", "libx265", "libsvtav1"):
        assert enc in _SALIENCY_DISPATCH, f"{enc!r} missing from _SALIENCY_DISPATCH"


# ---------------------------------------------------------------------------
# x265 — zones via -x265-params
# ---------------------------------------------------------------------------


def test_saliency_aware_encode_x265_emits_zones(tmp_path):
    """x265 dispatch: ``-x265-params zones=…`` appears in the argv."""
    request, _ = _make_request(tmp_path, "libx265")
    fake_model = tmp_path / "saliency_student_v1.onnx"
    fake_model.write_bytes(b"\x00")
    captured, runner = _capture_runner(tmp_path)

    cfg = SaliencyConfig(foreground_offset=-4, frame_samples=2, persist_qpfile=True)
    result = saliency_aware_encode(
        request,
        duration_frames=4,
        model_path=fake_model,
        config=cfg,
        encode_runner=runner,
        session_factory=_session_factory_for(128, 128),
    )

    assert result.exit_status == 0
    cmd = captured["cmd"]
    assert "-x265-params" in cmd, "expected -x265-params flag in ffmpeg argv"
    x265_val_idx = cmd.index("-x265-params") + 1
    assert cmd[x265_val_idx].startswith(
        "zones="
    ), f"expected zones=… value, got {cmd[x265_val_idx]!r}"


def test_saliency_aware_encode_x265_zones_covers_full_clip(tmp_path):
    """The zones string should span frame 0 to (duration-1)."""
    request, _ = _make_request(tmp_path, "libx265")
    fake_model = tmp_path / "saliency_student_v1.onnx"
    fake_model.write_bytes(b"\x00")
    captured, runner = _capture_runner(tmp_path)

    saliency_aware_encode(
        request,
        duration_frames=10,
        model_path=fake_model,
        config=SaliencyConfig(frame_samples=1, persist_qpfile=True),
        encode_runner=runner,
        session_factory=_session_factory_for(128, 128),
    )

    cmd = captured["cmd"]
    x265_val = cmd[cmd.index("-x265-params") + 1]
    # zones=0,9,q=<delta>
    assert x265_val.startswith(
        "zones=0,9,q="
    ), f"expected zones covering 10-frame clip (0..9), got {x265_val!r}"


def test_saliency_aware_encode_x265_does_not_emit_x264_params(tmp_path):
    """x265 path must not accidentally inject -x264-params."""
    request, _ = _make_request(tmp_path, "libx265")
    fake_model = tmp_path / "saliency_student_v1.onnx"
    fake_model.write_bytes(b"\x00")
    captured, runner = _capture_runner(tmp_path)

    saliency_aware_encode(
        request,
        duration_frames=1,
        model_path=fake_model,
        config=SaliencyConfig(frame_samples=1, persist_qpfile=True),
        encode_runner=runner,
        session_factory=_session_factory_for(128, 128),
    )

    assert "-x264-params" not in captured["cmd"]


# ---------------------------------------------------------------------------
# libaom-av1 — shared qpfile via patched FFmpeg -qpfile
# ---------------------------------------------------------------------------


def test_saliency_aware_encode_libaom_emits_qpfile(tmp_path):
    """libaom dispatch: top-level ``-qpfile <path>`` appears in the argv."""
    request, _ = _make_request(tmp_path, "libaom-av1")
    fake_model = tmp_path / "saliency_student_v1.onnx"
    fake_model.write_bytes(b"\x00")
    captured, runner = _capture_runner(tmp_path)

    cfg = SaliencyConfig(foreground_offset=-4, frame_samples=2, persist_qpfile=True)
    result = saliency_aware_encode(
        request,
        duration_frames=4,
        model_path=fake_model,
        config=cfg,
        encode_runner=runner,
        session_factory=_session_factory_for(128, 128),
    )

    assert result.exit_status == 0
    cmd = captured["cmd"]
    assert "-qpfile" in cmd, "expected -qpfile flag in ffmpeg argv"
    qpfile = Path(cmd[cmd.index("-qpfile") + 1])
    assert qpfile.exists()
    assert qpfile.name.endswith(".libaom-qpfile.txt")


def test_saliency_aware_encode_libaom_qpfile_has_16x16_granularity(tmp_path):
    """The written libaom qpfile should use the shared 16x16 MB grid."""
    request, _ = _make_request(tmp_path, "libaom-av1", width=64, height=64)
    fake_model = tmp_path / "saliency_student_v1.onnx"
    fake_model.write_bytes(b"\x00")
    captured, runner = _capture_runner(tmp_path)

    saliency_aware_encode(
        request,
        duration_frames=1,
        model_path=fake_model,
        config=SaliencyConfig(frame_samples=1, persist_qpfile=True),
        encode_runner=runner,
        session_factory=_session_factory_for(64, 64),
    )

    qpfile = Path(captured["cmd"][captured["cmd"].index("-qpfile") + 1])
    data_lines = qpfile.read_text(encoding="ascii").splitlines()[1:]
    assert len(data_lines) == 4, f"expected 4 MB rows (64/16), got {len(data_lines)}"
    for line in data_lines:
        cols = line.split()
        assert len(cols) == 4, f"expected 4 MB columns (64/16), got {len(cols)}: {line!r}"


def test_saliency_aware_encode_libaom_does_not_emit_encoder_private_roi_params(tmp_path):
    """libaom path must not use another encoder's private ROI channel."""
    request, _ = _make_request(tmp_path, "libaom-av1")
    fake_model = tmp_path / "saliency_student_v1.onnx"
    fake_model.write_bytes(b"\x00")
    captured, runner = _capture_runner(tmp_path)

    saliency_aware_encode(
        request,
        duration_frames=1,
        model_path=fake_model,
        config=SaliencyConfig(frame_samples=1, persist_qpfile=True),
        encode_runner=runner,
        session_factory=_session_factory_for(128, 128),
    )

    for forbidden in ("-x264-params", "-x265-params", "-svtav1-params", "-vvenc-params"):
        assert forbidden not in captured["cmd"], f"unexpected flag {forbidden!r} in libaom argv"


def test_saliency_aware_encode_libaom_cleans_ephemeral_qpfile(tmp_path):
    """Non-persisted libaom qpfiles are removed after the encode returns."""
    request, _ = _make_request(tmp_path, "libaom-av1")
    fake_model = tmp_path / "saliency_student_v1.onnx"
    fake_model.write_bytes(b"\x00")
    captured, runner = _capture_runner(tmp_path)

    saliency_aware_encode(
        request,
        duration_frames=1,
        model_path=fake_model,
        config=SaliencyConfig(frame_samples=1, persist_qpfile=False),
        encode_runner=runner,
        session_factory=_session_factory_for(128, 128),
    )

    qpfile = Path(captured["cmd"][captured["cmd"].index("-qpfile") + 1])
    assert not qpfile.exists()


# ---------------------------------------------------------------------------
# SVT-AV1 — x264-style qpfile via patched -qpfile bridge
# ---------------------------------------------------------------------------


def test_saliency_aware_encode_svtav1_emits_qp_file(tmp_path):
    """SVT-AV1 dispatch uses the patched top-level ``-qpfile`` option."""
    request, _ = _make_request(tmp_path, "libsvtav1")
    fake_model = tmp_path / "saliency_student_v1.onnx"
    fake_model.write_bytes(b"\x00")
    captured, runner = _capture_runner(tmp_path)

    cfg = SaliencyConfig(foreground_offset=-3, frame_samples=2, persist_qpfile=True)
    result = saliency_aware_encode(
        request,
        duration_frames=4,
        model_path=fake_model,
        config=cfg,
        encode_runner=runner,
        session_factory=_session_factory_for(128, 128),
    )

    assert result.exit_status == 0
    cmd = captured["cmd"]
    assert "-qpfile" in cmd
    assert "-svtav1-params" not in cmd
    qpfile = Path(cmd[cmd.index("-qpfile") + 1])
    assert qpfile == request.output.with_suffix(".svtav1-qpfile.txt")
    assert qpfile.read_text(encoding="ascii").startswith("0 I 0\n")


def test_saliency_aware_encode_svtav1_qpfile_has_16x16_granularity(tmp_path):
    """The bridge consumes frame headers and a 16×16 macroblock grid."""
    # 128×128 frame → 8×8 macroblock grid at 16×16.
    request, _ = _make_request(tmp_path, "libsvtav1", width=128, height=128)
    fake_model = tmp_path / "saliency_student_v1.onnx"
    fake_model.write_bytes(b"\x00")
    captured, runner = _capture_runner(tmp_path)

    saliency_aware_encode(
        request,
        duration_frames=1,
        model_path=fake_model,
        config=SaliencyConfig(frame_samples=1, persist_qpfile=True),
        encode_runner=runner,
        session_factory=_session_factory_for(128, 128),
    )

    qpfile = Path(captured["cmd"][captured["cmd"].index("-qpfile") + 1])
    lines = qpfile.read_text(encoding="ascii").splitlines()
    assert lines[0] == "0 I 0"
    assert lines[1:] == ["-2 -2 -2 -2 -2 -2 -2 -2"] * 8


def test_saliency_aware_encode_svtav1_does_not_emit_x265_params(tmp_path):
    """SVT-AV1 path must not accidentally inject -x265-params."""
    request, _ = _make_request(tmp_path, "libsvtav1")
    fake_model = tmp_path / "saliency_student_v1.onnx"
    fake_model.write_bytes(b"\x00")
    captured, runner = _capture_runner(tmp_path)

    saliency_aware_encode(
        request,
        duration_frames=1,
        model_path=fake_model,
        config=SaliencyConfig(frame_samples=1, persist_qpfile=True),
        encode_runner=runner,
        session_factory=_session_factory_for(128, 128),
    )

    assert "-x265-params" not in captured["cmd"]
    assert "-x264-params" not in captured["cmd"]


# ---------------------------------------------------------------------------
# libvvenc — no measured ROI channel (ADR-3386)
# ---------------------------------------------------------------------------


def test_saliency_aware_encode_vvenc_refused(tmp_path, monkeypatch):
    """Refuse libvvenc before invoking an encoder or writing an ROI sidecar."""
    monkeypatch.delenv("VMAFTUNE_SALIENCY_FALLBACK_OK", raising=False)
    request, _ = _make_request(tmp_path, "libvvenc")
    fake_model = tmp_path / "saliency_student_v1.onnx"
    fake_model.write_bytes(b"\x00")
    captured, runner = _capture_runner(tmp_path)

    assert "libvvenc" not in _SALIENCY_DISPATCH
    with pytest.raises(SaliencyUnsupportedEncoderError, match="libvvenc"):
        saliency_aware_encode(
            request,
            duration_frames=1,
            model_path=fake_model,
            config=SaliencyConfig(frame_samples=1, persist_qpfile=True),
            encode_runner=runner,
            session_factory=_session_factory_for(128, 128),
        )
    assert captured == {}
    assert not request.output.with_suffix(".vvenc-roi.csv").exists()


@pytest.mark.parametrize("size", [16, 128])
def test_saliency_aware_encode_svtav1_ephemeral_qpfile_removed(tmp_path, size):
    """The patched bridge accepts MB-sized input; temporary files are cleaned."""
    request, _ = _make_request(tmp_path, "libsvtav1", width=size, height=size)
    fake_model = tmp_path / "saliency_student_v1.onnx"
    fake_model.write_bytes(b"\x00")
    captured, runner = _capture_runner(tmp_path)
    saliency_aware_encode(
        request,
        duration_frames=1,
        model_path=fake_model,
        config=SaliencyConfig(frame_samples=1),
        encode_runner=runner,
        session_factory=_session_factory_for(max(size, 32), max(size, 32)),
    )
    qpfile = Path(captured["cmd"][captured["cmd"].index("-qpfile") + 1])
    assert qpfile.name.endswith(".svtav1-qpfile.txt")
    assert not qpfile.exists()


# ---------------------------------------------------------------------------
# Unknown encoder — graceful fallback
# ---------------------------------------------------------------------------


def test_saliency_aware_encode_unknown_encoder_falls_back_to_plain(tmp_path):
    """With allow_unsupported_encoder_fallback=True, unsupported encoders get a plain encode."""
    # libx264 is in the dispatch table, but we pass an unsupported name.
    request, _ = _make_request(tmp_path, "libx264")
    # Patch the encoder name to something not in the dispatch table.
    import dataclasses

    request = dataclasses.replace(request, encoder="libvpx-vp9")
    fake_model = tmp_path / "saliency_student_v1.onnx"
    fake_model.write_bytes(b"\x00")
    captured, runner = _capture_runner(tmp_path)

    saliency_aware_encode(
        request,
        duration_frames=1,
        model_path=fake_model,
        # ADR-0546: must explicitly opt in to fallback behaviour.
        config=SaliencyConfig(frame_samples=1, allow_unsupported_encoder_fallback=True),
        encode_runner=runner,
        session_factory=_session_factory_for(128, 128),
    )

    # None of the saliency-specific argv flags should appear.
    for flag in (
        "-qpfile",
        "-x264-params",
        "-x265-params",
        "-svtav1-params",
        "-vvenc-params",
    ):
        assert flag not in captured.get("cmd", [])


# ---------------------------------------------------------------------------
# ADR-0546 / saliency-tune-01: hard-fail on unsupported encoder (default)
# ---------------------------------------------------------------------------


def test_saliency_aware_encode_unsupported_encoder_raises_by_default(tmp_path):
    """Without opt-in fallback, an unsupported encoder raises SaliencyUnsupportedEncoderError.

    ADR-0546 (saliency-tune-01): the default posture is exit code 2, not a
    silent WARNING-and-continue.  Callers must explicitly opt in via
    allow_unsupported_encoder_fallback=True or VMAFTUNE_SALIENCY_FALLBACK_OK=1.
    """
    import dataclasses

    request, _ = _make_request(tmp_path, "libx264")
    request = dataclasses.replace(request, encoder="h264_nvenc")
    fake_model = tmp_path / "saliency_student_v1.onnx"
    fake_model.write_bytes(b"\x00")
    _, runner = _capture_runner(tmp_path)

    with pytest.raises(SaliencyUnsupportedEncoderError) as exc_info:
        saliency_aware_encode(
            request,
            duration_frames=1,
            model_path=fake_model,
            config=SaliencyConfig(frame_samples=1),
            encode_runner=runner,
            session_factory=_session_factory_for(128, 128),
        )

    err = exc_info.value
    # Exit code must be 2 (inherits from SystemExit).
    assert err.code == (
        f"vmaf-tune: saliency ROI is not implemented for encoder 'h264_nvenc'.\n"
        f"Supported encoders: {', '.join(sorted(_SALIENCY_DISPATCH))}\n"
        "To accept a plain encode without ROI bias, pass --saliency-fallback-plain "
        "or set VMAFTUNE_SALIENCY_FALLBACK_OK=1."
    )
    assert err.encoder == "h264_nvenc"
    assert "h264_nvenc" not in err.supported


def test_saliency_aware_encode_unsupported_encoder_env_override_allows_fallback(
    tmp_path, monkeypatch
):
    """VMAFTUNE_SALIENCY_FALLBACK_OK=1 re-enables the old graceful-fallback behaviour.

    ADR-0546: the env var is the non-flag opt-in path for callers that cannot
    easily modify SaliencyConfig (e.g. external scripts or CI wrappers).
    """
    import dataclasses

    request, _ = _make_request(tmp_path, "libx264")
    request = dataclasses.replace(request, encoder="libvpx-vp9")
    fake_model = tmp_path / "saliency_student_v1.onnx"
    fake_model.write_bytes(b"\x00")
    captured, runner = _capture_runner(tmp_path)

    monkeypatch.setenv("VMAFTUNE_SALIENCY_FALLBACK_OK", "1")

    # Must NOT raise — the env var unlocks the fallback path.
    saliency_aware_encode(
        request,
        duration_frames=1,
        model_path=fake_model,
        config=SaliencyConfig(frame_samples=1),
        encode_runner=runner,
        session_factory=_session_factory_for(128, 128),
    )

    # Encoder argv must contain no ROI-specific flags.
    for flag in (
        "-qpfile",
        "-x264-params",
        "-x265-params",
        "-svtav1-params",
        "-vvenc-params",
    ):
        assert flag not in captured.get("cmd", [])
