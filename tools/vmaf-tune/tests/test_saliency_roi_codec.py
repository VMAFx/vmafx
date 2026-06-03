# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""End-to-end dispatch tests for saliency-aware ROI encoding.

One test per target encoder (x265 / SVT-AV1 / libaom / libvvenc) verifying that
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

from vmaftune.encode import EncodeRequest  # noqa: E402
from vmaftune.saliency import (  # noqa: E402
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
        captured["cmd"] = list(cmd)
        out = Path(cmd[-1])
        out.write_bytes(b"\x00\x01")

        class _Done:
            returncode = 0
            stdout = ""
            stderr = "ffmpeg version 6.1.1\nlibx265 HEVC encoder"

        return _Done()

    return captured, _runner


# ---------------------------------------------------------------------------
# Dispatch-table coverage
# ---------------------------------------------------------------------------


def test_saliency_dispatch_table_contains_roi_targets():
    """_SALIENCY_DISPATCH must wire every saliency ROI target."""
    for enc in ("libx264", "libaom-av1", "libx265", "libsvtav1", "libvvenc"):
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
# SVT-AV1 — qpmap file via -svtav1-params
# ---------------------------------------------------------------------------


def test_saliency_aware_encode_svtav1_emits_qp_file(tmp_path):
    """SVT-AV1 dispatch: ``-svtav1-params qp-file=…`` appears in the argv."""
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
    assert "-svtav1-params" in cmd, "expected -svtav1-params flag in ffmpeg argv"
    svt_val_idx = cmd.index("-svtav1-params") + 1
    assert cmd[svt_val_idx].startswith(
        "qp-file="
    ), f"expected qp-file=… value, got {cmd[svt_val_idx]!r}"


def test_saliency_aware_encode_svtav1_qpmap_file_has_64x64_granularity(tmp_path):
    """The written qpmap file should use 64×64 super-block grid rows."""
    # 128×128 frame → 2×2 super-block grid at 64×64.
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

    svt_val = captured["cmd"][captured["cmd"].index("-svtav1-params") + 1]
    qpmap_path = Path(svt_val.split("qp-file=", 1)[1])
    assert qpmap_path.exists()
    text = qpmap_path.read_text(encoding="ascii")
    # Non-blank lines are super-block rows; each should have 2 space-separated values.
    data_lines = [ln for ln in text.splitlines() if ln.strip()]
    assert len(data_lines) == 2, f"expected 2 SB rows (128/64), got {len(data_lines)}"
    for line in data_lines:
        cols = line.split()
        assert len(cols) == 2, f"expected 2 SB columns (128/64), got {len(cols)}: {line!r}"


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
# libvvenc — ROI CSV via -vvenc-params ROIFile
# ---------------------------------------------------------------------------


def test_saliency_aware_encode_vvenc_emits_roi_file(tmp_path):
    """VVenC dispatch: ``-vvenc-params ROIFile=…`` appears in the argv."""
    request, _ = _make_request(tmp_path, "libvvenc")
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
    assert "-vvenc-params" in cmd, "expected -vvenc-params flag in ffmpeg argv"
    vvenc_val_idx = cmd.index("-vvenc-params") + 1
    assert cmd[vvenc_val_idx].startswith(
        "ROIFile="
    ), f"expected ROIFile=… value, got {cmd[vvenc_val_idx]!r}"


def test_saliency_aware_encode_vvenc_roi_csv_is_comma_separated(tmp_path):
    """The written VVenC ROI CSV must use commas, not spaces."""
    # 128×128 frame → 2×2 CTU grid at 64×64.
    request, _ = _make_request(tmp_path, "libvvenc", width=128, height=128)
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

    vvenc_val = captured["cmd"][captured["cmd"].index("-vvenc-params") + 1]
    roi_path = Path(vvenc_val.split("ROIFile=", 1)[1])
    assert roi_path.exists()
    text = roi_path.read_text(encoding="ascii")
    data_lines = [ln for ln in text.splitlines() if ln.strip()]
    assert len(data_lines) == 2, f"expected 2 CTU rows (128/64), got {len(data_lines)}"
    for line in data_lines:
        assert "," in line, f"VVenC ROI row must be comma-separated, got: {line!r}"


def test_saliency_aware_encode_vvenc_does_not_emit_x265_or_svt_params(tmp_path):
    """VVenC path must not accidentally inject x264/x265/svtav1 params."""
    request, _ = _make_request(tmp_path, "libvvenc")
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

    for forbidden in ("-x264-params", "-x265-params", "-svtav1-params"):
        assert forbidden not in captured["cmd"], f"unexpected flag {forbidden!r} in vvenc argv"


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
    for flag in ("-qpfile", "-x264-params", "-x265-params", "-svtav1-params", "-vvenc-params"):
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
    for flag in ("-qpfile", "-x264-params", "-x265-params", "-svtav1-params", "-vvenc-params"):
        assert flag not in captured.get("cmd", [])
