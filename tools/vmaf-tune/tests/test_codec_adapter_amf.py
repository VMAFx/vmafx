# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for the AMD AMF codec adapters (h264_amf / hevc_amf / av1_amf).

Mocks ``subprocess.run`` so the suite needs neither an AMD GPU nor
an ffmpeg build. Covers:

- Registry wiring for all three encoders.
- Preset-name -> AMF quality compression (7 -> 3 levels).
- QP range validation (Phase A informative window 15..40).
- ``ensure_amf_available`` failure path when the encoder is missing.
- ``extra_params`` argv shape (-quality / -rc cqp / -qp_i / -qp_p).
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

# Make src/ importable without an editable install.
_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.codec_adapters import (  # noqa: E402
    AV1AMFAdapter,
    H264AMFAdapter,
    HEVCAMFAdapter,
    get_adapter,
    known_codecs,
)
from vmaftune.codec_adapters._amf_common import (  # noqa: E402
    ensure_amf_available,
    map_preset_to_amf_quality,
)


class _FakeCompleted:
    """Minimal stand-in for ``subprocess.CompletedProcess``."""

    def __init__(self, returncode: int, stdout: str = "", stderr: str = ""):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


_AMF_NAMES = ("h264_amf", "hevc_amf", "av1_amf")


@pytest.mark.parametrize("name", _AMF_NAMES)
def test_amf_adapters_registered(name):
    assert name in known_codecs()
    a = get_adapter(name)
    assert a.encoder == name
    assert a.quality_knob == "qp"
    assert a.invert_quality is True
    assert a.quality_range == (15, 40)


@pytest.mark.parametrize(
    "cls,encoder",
    [
        (H264AMFAdapter, "h264_amf"),
        (HEVCAMFAdapter, "hevc_amf"),
        (AV1AMFAdapter, "av1_amf"),
    ],
)
def test_amf_adapter_classes_pin_encoder(cls, encoder):
    assert cls().encoder == encoder
    assert cls().name == encoder


@pytest.mark.parametrize(
    "preset,expected",
    [
        # Slow rungs collapse to 'quality'.
        ("placebo", "quality"),
        ("slowest", "quality"),
        ("slower", "quality"),
        ("slow", "quality"),
        # Default rung.
        ("medium", "balanced"),
        # Fast rungs collapse to 'speed'.
        ("fast", "speed"),
        ("faster", "speed"),
        ("veryfast", "speed"),
        ("superfast", "speed"),
        ("ultrafast", "speed"),
    ],
)
def test_preset_compression_7_into_3(preset, expected):
    assert map_preset_to_amf_quality(preset) == expected


def test_preset_compression_rejects_unknown():
    with pytest.raises(ValueError, match="unknown AMF preset"):
        map_preset_to_amf_quality("nonsense")


@pytest.mark.parametrize("name", _AMF_NAMES)
def test_amf_validate_accepts_known_pairs(name):
    a = get_adapter(name)
    a.validate("medium", 23)
    a.validate("slow", 30)
    a.validate("ultrafast", 40)
    a.validate("placebo", 15)


@pytest.mark.parametrize("name", _AMF_NAMES)
def test_amf_validate_rejects_bad_preset(name):
    a = get_adapter(name)
    with pytest.raises(ValueError, match="unknown AMF preset"):
        a.validate("does-not-exist", 23)


@pytest.mark.parametrize("name", _AMF_NAMES)
def test_amf_validate_rejects_qp_out_of_range(name):
    a = get_adapter(name)
    with pytest.raises(ValueError, match="outside Phase A range"):
        a.validate("medium", 14)
    with pytest.raises(ValueError, match="outside Phase A range"):
        a.validate("medium", 41)
    with pytest.raises(ValueError, match="outside Phase A range"):
        a.validate("medium", 100)
    with pytest.raises(ValueError, match="outside Phase A range"):
        a.validate("medium", -1)


@pytest.mark.parametrize("name", _AMF_NAMES)
def test_amf_extra_params_shape(name):
    a = get_adapter(name)
    params = a.extra_params("medium", 23)
    # Argv ordering matters for ffmpeg; assert the exact tuple.
    assert params == (
        "-quality",
        "balanced",
        "-rc",
        "cqp",
        "-qp_i",
        "23",
        "-qp_p",
        "23",
    )


def test_amf_extra_params_compresses_slow_to_quality():
    a = get_adapter("h264_amf")
    assert a.extra_params("placebo", 18)[1] == "quality"
    assert a.extra_params("slower", 18)[1] == "quality"


def test_amf_extra_params_compresses_fast_to_speed():
    a = get_adapter("hevc_amf")
    assert a.extra_params("ultrafast", 30)[1] == "speed"
    assert a.extra_params("veryfast", 30)[1] == "speed"


def test_ensure_amf_available_succeeds_when_encoder_listed():
    def fake_run(cmd, capture_output, text, check):
        # ffmpeg -encoders returns a tabular list; only the encoder
        # name needs to substring-match.
        return _FakeCompleted(
            returncode=0,
            stdout=" V..... h264_amf             AMD AMF H.264 encoder\n",
        )

    # Must not raise.
    ensure_amf_available(encoder="h264_amf", runner=fake_run)


def test_ensure_amf_available_raises_when_encoder_missing():
    def fake_run(cmd, capture_output, text, check):
        return _FakeCompleted(
            returncode=0,
            stdout=" V..... libx264              libx264 H.264 / AVC\n",
        )

    with pytest.raises(RuntimeError, match="unavailable"):
        ensure_amf_available(encoder="h264_amf", runner=fake_run)


def test_ensure_amf_available_raises_on_ffmpeg_failure():
    def fake_run(cmd, capture_output, text, check):
        return _FakeCompleted(returncode=127, stdout="", stderr="not found")

    with pytest.raises(RuntimeError, match="unavailable"):
        ensure_amf_available(encoder="hevc_amf", runner=fake_run)


@pytest.mark.parametrize("name", _AMF_NAMES)
def test_ensure_amf_available_each_codec(name):
    """All three codec names take the same probe code path."""

    def fake_run(cmd, capture_output, text, check):
        listing = (
            " V..... h264_amf             AMD AMF H.264 encoder\n"
            " V..... hevc_amf             AMD AMF HEVC encoder\n"
            " V..... av1_amf              AMD AMF AV1 encoder\n"
        )
        return _FakeCompleted(returncode=0, stdout=listing)

    ensure_amf_available(encoder=name, runner=fake_run)
