# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""VideoToolbox codec adapter smoke tests (ADR-0283).

Mocks ``subprocess.run`` so the suite has no runtime dependency on a
macOS host or a working VideoToolbox encoder. Production callers run
on Apple Silicon / Intel-Mac-with-T2; the test gate only verifies the
adapter contract and the encode-driver argv shape.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.codec_adapters import (
    H264VideoToolboxAdapter,
    HEVCVideoToolboxAdapter,
    get_adapter,
    known_codecs,
)
from vmaftune.codec_adapters._videotoolbox_common import (
    VIDEOTOOLBOX_PRESETS,
    VIDEOTOOLBOX_QUALITY_RANGE,
    preset_to_realtime,
    validate_videotoolbox,
)
from vmaftune.encode import (
    EncodeRequest,
    build_ffmpeg_command,
    run_encode,
)


class _FakeCompleted:
    def __init__(self, returncode: int, stdout: str = "", stderr: str = ""):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


def test_h264_videotoolbox_adapter_contract():
    a = H264VideoToolboxAdapter()
    assert a.name == "h264_videotoolbox"
    assert a.encoder == "h264_videotoolbox"
    assert a.quality_knob == "q:v"
    assert a.quality_range == (0, 100)
    assert a.invert_quality is False  # higher q:v = higher quality


def test_hevc_videotoolbox_adapter_contract():
    a = HEVCVideoToolboxAdapter()
    assert a.name == "hevc_videotoolbox"
    assert a.encoder == "hevc_videotoolbox"
    assert a.quality_knob == "q:v"
    assert a.quality_range == (0, 100)
    assert a.invert_quality is False


def test_videotoolbox_registered_in_registry():
    assert "h264_videotoolbox" in known_codecs()
    assert "hevc_videotoolbox" in known_codecs()
    assert get_adapter("h264_videotoolbox").encoder == "h264_videotoolbox"
    assert get_adapter("hevc_videotoolbox").encoder == "hevc_videotoolbox"


def test_videotoolbox_preset_to_realtime_mapping():
    # Fast presets → realtime=1; quality presets → realtime=0
    for fast in ("ultrafast", "superfast", "veryfast", "faster", "fast"):
        assert preset_to_realtime(fast) == "1"
    for slow in ("medium", "slow", "slower", "veryslow"):
        assert preset_to_realtime(slow) == "0"
    with pytest.raises(ValueError):
        preset_to_realtime("not-a-preset")


def test_videotoolbox_validate_accepts_full_range():
    a = H264VideoToolboxAdapter()
    a.validate("medium", 50)
    a.validate("ultrafast", 0)
    a.validate("veryslow", 100)


def test_videotoolbox_validate_rejects_out_of_range():
    a = HEVCVideoToolboxAdapter()
    with pytest.raises(ValueError):
        a.validate("medium", -1)
    with pytest.raises(ValueError):
        a.validate("medium", 101)
    with pytest.raises(ValueError):
        a.validate("nope", 50)


def test_validate_videotoolbox_helper_matches_constants():
    lo, hi = VIDEOTOOLBOX_QUALITY_RANGE
    validate_videotoolbox(VIDEOTOOLBOX_PRESETS[0], lo)
    validate_videotoolbox(VIDEOTOOLBOX_PRESETS[-1], hi)
    with pytest.raises(ValueError):
        validate_videotoolbox("medium", hi + 1)


def test_h264_vt_encode_argv_shape_via_mock(tmp_path: Path):
    """Smoke: build_ffmpeg_command + run_encode with a mocked subprocess."""
    captured: dict = {}

    def fake_run(cmd, capture_output, text, check):
        captured["cmd"] = cmd
        Path(cmd[-1]).write_bytes(b"\x00" * 1024)
        return _FakeCompleted(returncode=0, stderr="ffmpeg version 6.1.1\n")

    src = tmp_path / "ref.yuv"
    src.write_bytes(b"\x80" * 1024)
    out = tmp_path / "out.mkv"

    req = EncodeRequest(
        source=src,
        width=1920,
        height=1080,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder="h264_videotoolbox",
        preset="medium",
        crf=50,
        output=out,
    )
    # Direct command builder check.
    cmd = build_ffmpeg_command(req)
    assert "h264_videotoolbox" in cmd
    assert "-c:v" in cmd
    # Driver path with the mock.
    res = run_encode(req, runner=fake_run)
    assert res.exit_status == 0
    assert "h264_videotoolbox" in captured["cmd"]


def test_hevc_vt_encode_argv_shape_via_mock(tmp_path: Path):
    captured: dict = {}

    def fake_run(cmd, capture_output, text, check):
        captured["cmd"] = cmd
        Path(cmd[-1]).write_bytes(b"\x00" * 1024)
        return _FakeCompleted(returncode=0, stderr="ffmpeg version 6.1.1\n")

    src = tmp_path / "ref.yuv"
    src.write_bytes(b"\x80" * 1024)
    out = tmp_path / "out.mkv"
    req = EncodeRequest(
        source=src,
        width=1920,
        height=1080,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder="hevc_videotoolbox",
        preset="fast",
        crf=60,
        output=out,
    )
    cmd = build_ffmpeg_command(req)
    assert "hevc_videotoolbox" in cmd
    res = run_encode(req, runner=fake_run)
    assert res.exit_status == 0
    assert "hevc_videotoolbox" in captured["cmd"]
