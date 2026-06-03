# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for the Intel QSV codec adapters (ADR-0281).

The QSV encoders share preset vocabulary, quality knob, and ICQ range,
so the suite parametrises across all three adapter classes. Subprocess
boundaries are mocked — no `ffmpeg` binary is required.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

# Make src/ importable without an editable install (mirrors test_corpus.py).
_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.codec_adapters import (  # noqa: E402
    Av1QsvAdapter,
    H264QsvAdapter,
    HevcQsvAdapter,
    get_adapter,
    known_codecs,
)
from vmaftune.codec_adapters._qsv_common import (  # noqa: E402
    QSV_PRESETS,
    QSV_QUALITY_DEFAULT,
    QSV_QUALITY_RANGE,
    ffmpeg_supports_encoder,
    preset_to_qsv,
    require_qsv_encoder,
    validate_global_quality,
)


class _FakeCompleted:
    def __init__(self, returncode: int = 0, stdout: str = "", stderr: str = ""):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


# --- Adapter parametrisation ----------------------------------------

_ADAPTER_CASES = [
    ("h264_qsv", H264QsvAdapter, "h264_qsv"),
    ("hevc_qsv", HevcQsvAdapter, "hevc_qsv"),
    ("av1_qsv", Av1QsvAdapter, "av1_qsv"),
]


@pytest.mark.parametrize("name,cls,encoder", _ADAPTER_CASES)
def test_adapter_shape(name: str, cls: type, encoder: str) -> None:
    adapter = cls()
    assert adapter.name == name
    assert adapter.encoder == encoder
    assert adapter.quality_knob == "global_quality"
    assert adapter.quality_range == QSV_QUALITY_RANGE
    assert adapter.quality_default == QSV_QUALITY_DEFAULT
    assert adapter.invert_quality is True
    assert adapter.presets == QSV_PRESETS


@pytest.mark.parametrize("name,cls,encoder", _ADAPTER_CASES)
def test_registry_lookup(name: str, cls: type, encoder: str) -> None:
    adapter = get_adapter(name)
    assert isinstance(adapter, cls)


def test_registry_includes_three_qsv_adapters() -> None:
    codecs = known_codecs()
    for name in ("h264_qsv", "hevc_qsv", "av1_qsv"):
        assert name in codecs


# --- Preset identity mapping ----------------------------------------


@pytest.mark.parametrize("preset", list(QSV_PRESETS))
def test_preset_to_qsv_identity(preset: str) -> None:
    # QSV uses the x264-style names verbatim — the mapping is identity.
    assert preset_to_qsv(preset) == preset


def test_preset_to_qsv_rejects_unknown() -> None:
    with pytest.raises(ValueError, match="unknown QSV preset"):
        preset_to_qsv("ultrafast")  # libx264-only level
    with pytest.raises(ValueError, match="unknown QSV preset"):
        preset_to_qsv("placebo")
    with pytest.raises(ValueError, match="unknown QSV preset"):
        preset_to_qsv("MEDIUM")  # case-sensitive


@pytest.mark.parametrize("name,cls,encoder", _ADAPTER_CASES)
def test_adapter_validate_accepts_canonical_inputs(name: str, cls: type, encoder: str) -> None:
    adapter = cls()
    for preset in QSV_PRESETS:
        adapter.validate(preset, QSV_QUALITY_DEFAULT)


@pytest.mark.parametrize("name,cls,encoder", _ADAPTER_CASES)
def test_adapter_validate_rejects_unknown_preset(name: str, cls: type, encoder: str) -> None:
    adapter = cls()
    with pytest.raises(ValueError):
        adapter.validate("ultrafast", 23)


# --- global_quality range validation --------------------------------


@pytest.mark.parametrize("value", [1, 23, 51])
def test_validate_global_quality_accepts_in_range(value: int) -> None:
    validate_global_quality(value)


@pytest.mark.parametrize("value", [-1, 0, 52, 100, 1000])
def test_validate_global_quality_rejects_out_of_range(value: int) -> None:
    with pytest.raises(ValueError, match=r"outside ICQ range"):
        validate_global_quality(value)


@pytest.mark.parametrize("name,cls,encoder", _ADAPTER_CASES)
def test_adapter_validate_rejects_out_of_range_quality(name: str, cls: type, encoder: str) -> None:
    adapter = cls()
    with pytest.raises(ValueError):
        adapter.validate("medium", 0)
    with pytest.raises(ValueError):
        adapter.validate("medium", 52)


# --- FFmpeg encoder probe -------------------------------------------


def _fake_runner(stdout: str, returncode: int = 0):
    def _runner(cmd, capture_output, text, check):  # noqa: ARG001
        return _FakeCompleted(returncode=returncode, stdout=stdout)

    return _runner


def test_ffmpeg_supports_encoder_true_when_listed() -> None:
    listing = (
        "Encoders:\n"
        " V..... libx264              libx264 H.264 / AVC\n"
        " V..... h264_qsv             H.264 / AVC (Intel Quick Sync Video)\n"
        " V..... hevc_qsv             HEVC (Intel Quick Sync Video)\n"
    )
    runner = _fake_runner(listing)
    assert ffmpeg_supports_encoder("h264_qsv", runner=runner) is True
    assert ffmpeg_supports_encoder("hevc_qsv", runner=runner) is True
    assert ffmpeg_supports_encoder("av1_qsv", runner=runner) is False


def test_ffmpeg_supports_encoder_false_on_empty_listing() -> None:
    runner = _fake_runner("Encoders:\n")
    assert ffmpeg_supports_encoder("h264_qsv", runner=runner) is False


def test_ffmpeg_supports_encoder_handles_missing_binary() -> None:
    def _missing_runner(*_a, **_kw):
        raise FileNotFoundError("ffmpeg")

    assert ffmpeg_supports_encoder("h264_qsv", runner=_missing_runner) is False


def test_require_qsv_encoder_raises_when_unsupported() -> None:
    runner = _fake_runner(" V..... libx264              libx264 H.264 / AVC\n")
    with pytest.raises(RuntimeError, match="ffmpeg does not advertise 'h264_qsv'"):
        require_qsv_encoder("h264_qsv", runner=runner)


def test_require_qsv_encoder_passes_when_supported() -> None:
    runner = _fake_runner(" V..... av1_qsv              AV1 (Intel Quick Sync Video)\n")
    # Must not raise.
    require_qsv_encoder("av1_qsv", runner=runner)
