# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""``av1_videotoolbox`` placeholder-adapter tests (ADR-0339).

The adapter ships in placeholder mode: it registers in
``ADAPTER_REGISTRY`` but ``validate`` raises
:class:`Av1VideoToolboxUnavailableError` until a runtime probe of
``ffmpeg -h encoder=av1_videotoolbox`` confirms the encoder exists.
These tests exercise both halves of the gate via an injected
``runner`` callable so the suite stays hermetic on any host (no real
``ffmpeg`` invocation, no macOS dependency).
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.codec_adapters import (
    Av1VideoToolboxAdapter,
    Av1VideoToolboxUnavailableError,
    get_adapter,
    known_codecs,
)
from vmaftune.codec_adapters import av1_videotoolbox as mod


class _FakeCompleted:
    def __init__(self, returncode: int, stdout: str = "", stderr: str = ""):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


# Realistic FFmpeg outputs captured against n8.1 master ``8518599cd1``
# (2026-05-09): the not-recognized message FFmpeg currently prints
# for av1_videotoolbox, and a synthesised encoder-help block matching
# the shape FFmpeg emits for the existing h264_videotoolbox encoder.
_FFMPEG_NOT_RECOGNIZED = "Codec 'av1_videotoolbox' is not recognized by FFmpeg.\n"
_FFMPEG_ENCODER_HELP = (
    "Encoder av1_videotoolbox [VideoToolbox AV1 Encoder]:\n"
    "    General capabilities: dr1 hardware\n"
    "    Threading capabilities: none\n"
    "    Supported pixel formats: videotoolbox_vld nv12 yuv420p\n"
    "av1_videotoolbox AVOptions:\n"
    "  -realtime          <int>    E..V....... Hint that encoding should happen in real-time\n"
    "  -q                 <int>    E..V....... Constant quality (0..100)\n"
)


def _runner_returning(blob_stdout: str, *, returncode: int = 0):
    """Build a fake ``subprocess.run`` that always returns this blob."""

    captured: dict = {}

    def fake_run(cmd, *, capture_output, text, timeout):
        captured["cmd"] = cmd
        return _FakeCompleted(returncode=returncode, stdout=blob_stdout, stderr="")

    fake_run.captured = captured  # type: ignore[attr-defined]
    return fake_run


# --------------------------------------------------------------- contract


def test_av1_videotoolbox_adapter_contract():
    a = Av1VideoToolboxAdapter()
    assert a.name == "av1_videotoolbox"
    assert a.encoder == "av1_videotoolbox"
    assert a.quality_knob == "q:v"
    assert a.quality_range == (0, 100)
    assert a.invert_quality is False
    assert a.supports_runtime is False  # placeholder default
    # Cache key is bumped on activation; placeholder stays at the
    # ``0-placeholder`` sentinel so corpus rows produced before
    # activation are recognisable.
    assert a.adapter_version == "0-placeholder"


def test_av1_videotoolbox_registered_in_registry():
    assert "av1_videotoolbox" in known_codecs()
    assert get_adapter("av1_videotoolbox").encoder == "av1_videotoolbox"


# --------------------------------------------------------- probe (inactive)


def test_probe_returns_false_when_ffmpeg_says_not_recognized():
    fake = _runner_returning(_FFMPEG_NOT_RECOGNIZED, returncode=1)
    assert mod.probe_av1_videotoolbox_available(ffmpeg_bin="/usr/bin/ffmpeg", runner=fake) is False


def test_probe_returns_false_when_ffmpeg_missing(monkeypatch):
    monkeypatch.setattr(mod.shutil, "which", lambda _name: None)
    assert mod.probe_av1_videotoolbox_available() is False


def test_probe_returns_false_on_oserror():
    def boom(*_args, **_kwargs):
        raise OSError("no ffmpeg today")

    assert mod.probe_av1_videotoolbox_available(ffmpeg_bin="/usr/bin/ffmpeg", runner=boom) is False


def test_validate_raises_unavailable_when_probe_inactive(monkeypatch):
    monkeypatch.setattr(mod, "probe_av1_videotoolbox_available", lambda **_kw: False)
    a = Av1VideoToolboxAdapter()
    with pytest.raises(Av1VideoToolboxUnavailableError, match="ADR-0339"):
        a.validate("medium", 50)


def test_ffmpeg_codec_args_refuses_emit_while_inactive(monkeypatch):
    """No-guessing rule: argv is not emitted until the probe activates."""
    monkeypatch.setattr(mod, "probe_av1_videotoolbox_available", lambda **_kw: False)
    a = Av1VideoToolboxAdapter()
    with pytest.raises(Av1VideoToolboxUnavailableError):
        a.ffmpeg_codec_args("medium", 50)


# ----------------------------------------------------------- probe (active)


def test_probe_returns_true_on_encoder_help_output():
    fake = _runner_returning(_FFMPEG_ENCODER_HELP, returncode=0)
    assert mod.probe_av1_videotoolbox_available(ffmpeg_bin="/usr/bin/ffmpeg", runner=fake) is True


def test_validate_passes_when_probe_activates(monkeypatch):
    monkeypatch.setattr(mod, "probe_av1_videotoolbox_available", lambda **_kw: True)
    a = Av1VideoToolboxAdapter()
    a.validate("medium", 50)  # no raise
    a.validate("ultrafast", 0)
    a.validate("veryslow", 100)


def test_validate_rejects_out_of_range_when_active(monkeypatch):
    monkeypatch.setattr(mod, "probe_av1_videotoolbox_available", lambda **_kw: True)
    a = Av1VideoToolboxAdapter()
    with pytest.raises(ValueError):
        a.validate("medium", -1)
    with pytest.raises(ValueError):
        a.validate("medium", 101)
    with pytest.raises(ValueError):
        a.validate("not-a-preset", 50)


def test_ffmpeg_codec_args_emits_correct_argv_when_active(monkeypatch):
    monkeypatch.setattr(mod, "probe_av1_videotoolbox_available", lambda **_kw: True)
    a = Av1VideoToolboxAdapter()
    argv = a.ffmpeg_codec_args("medium", 60)
    assert argv == ["-c:v", "av1_videotoolbox", "-realtime", "0", "-q:v", "60"]
    # Fast preset → realtime=1
    argv_fast = a.ffmpeg_codec_args("ultrafast", 40)
    assert argv_fast == ["-c:v", "av1_videotoolbox", "-realtime", "1", "-q:v", "40"]
