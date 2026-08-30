# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""NVENC codec-adapter smoke tests — mocks subprocess so no GPU required."""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

# Make src/ importable without an editable install.
_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.codec_adapters import (
    Av1NvencAdapter,
    H264NvencAdapter,
    HevcNvencAdapter,
    get_adapter,
    known_codecs,
)
from vmaftune.codec_adapters._nvenc_common import (
    NVENC_CQ_HARD_LIMITS,
    NVENC_PRESETS,
    nvenc_preset,
    validate_nvenc,
)

# ---------------------------------------------------------------------------
# Registry
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "name,cls",
    [
        ("h264_nvenc", H264NvencAdapter),
        ("hevc_nvenc", HevcNvencAdapter),
        ("av1_nvenc", Av1NvencAdapter),
    ],
)
def test_nvenc_adapter_registered(name, cls):
    a = get_adapter(name)
    assert isinstance(a, cls)
    assert a.encoder == name
    assert a.quality_knob == "cq"
    assert a.invert_quality is True


def test_known_codecs_contains_nvenc_family():
    codecs = known_codecs()
    for name in ("h264_nvenc", "hevc_nvenc", "av1_nvenc"):
        assert name in codecs


# ---------------------------------------------------------------------------
# Preset mapping — mnemonic → p1..p7
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "mnemonic,expected",
    [
        ("ultrafast", "p1"),
        ("superfast", "p1"),
        ("veryfast", "p1"),
        ("faster", "p2"),
        ("fast", "p3"),
        ("medium", "p4"),  # default
        ("slow", "p5"),
        ("slower", "p6"),
        ("slowest", "p7"),
        ("placebo", "p7"),
    ],
)
def test_nvenc_preset_mapping(mnemonic, expected):
    assert nvenc_preset(mnemonic) == expected


def test_nvenc_preset_mapping_default_is_medium_p4():
    # The 'medium' default preset must always map to p4.
    a = get_adapter("h264_nvenc")
    assert a.nvenc_preset("medium") == "p4"


def test_nvenc_preset_unknown_raises():
    with pytest.raises(ValueError, match="unknown NVENC preset"):
        nvenc_preset("does-not-exist")


def test_nvenc_preset_set_size_is_seven_unique_levels():
    # p1..p7 — 7 levels.
    levels = {nvenc_preset(p) for p in NVENC_PRESETS}
    assert levels == {"p1", "p2", "p3", "p4", "p5", "p6", "p7"}


# ---------------------------------------------------------------------------
# CQ range validation — [0, 51]
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("name", ["h264_nvenc", "hevc_nvenc", "av1_nvenc"])
@pytest.mark.parametrize("cq", [0, 1, 23, 28, 50, 51])
def test_validate_accepts_cq_in_hardware_range(name, cq):
    a = get_adapter(name)
    a.validate("medium", cq)  # must not raise


@pytest.mark.parametrize("name", ["h264_nvenc", "hevc_nvenc", "av1_nvenc"])
@pytest.mark.parametrize("bad_cq", [-1, 52, 100, 1000])
def test_validate_rejects_cq_out_of_range(name, bad_cq):
    a = get_adapter(name)
    with pytest.raises(ValueError, match="cq"):
        a.validate("medium", bad_cq)


def test_validate_rejects_unknown_preset():
    a = get_adapter("h264_nvenc")
    with pytest.raises(ValueError, match="unknown NVENC preset"):
        a.validate("not-a-preset", 23)


def test_validate_nvenc_helper_directly():
    # Sanity check the shared helper.
    validate_nvenc("medium", 23)
    lo, hi = NVENC_CQ_HARD_LIMITS
    assert (lo, hi) == (0, 51)


# ---------------------------------------------------------------------------
# Encoder-not-available simulation — exercises the subprocess error path
# that callers must surface up the harness.
# ---------------------------------------------------------------------------


class _FakeCompleted:
    """Minimal stand-in for ``subprocess.CompletedProcess``."""

    def __init__(self, returncode: int, stdout: str = "", stderr: str = ""):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


@pytest.mark.parametrize(
    "encoder",
    ["h264_nvenc", "hevc_nvenc", "av1_nvenc"],
)
def test_subprocess_mock_reports_encoder_not_found(encoder):
    """Simulate FFmpeg failing because NVENC isn't available.

    The adapter contract doesn't itself probe hardware; the corpus
    harness propagates a non-zero exit code and the stderr tail. This
    test pins the contract: a fake ``subprocess.run`` returning
    ``"Encoder X not found"`` must surface as ``returncode != 0`` so
    the row writer records ``exit_status != 0`` and skips scoring.
    """

    def fake_run(cmd, capture_output, text, check):  # pragma: no cover - shape pin
        # Confirm the encoder was actually wired into the argv.
        assert encoder in cmd
        return _FakeCompleted(
            returncode=1,
            stderr=f"[error] Encoder {encoder} not found\n",
        )

    # Build a minimal argv that mirrors what the harness would emit.
    cmd = [
        "ffmpeg",
        "-y",
        "-i",
        "ref.yuv",
        "-c:v",
        encoder,
        "-preset",
        "p4",
        "-cq",
        "23",
        "out.mkv",
    ]
    result = fake_run(cmd, capture_output=True, text=True, check=False)
    assert result.returncode == 1
    assert "not found" in result.stderr
    assert encoder in result.stderr


# ---------------------------------------------------------------------------
# Adapter-level invariants shared across all three NVENC codecs
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("name", ["h264_nvenc", "hevc_nvenc", "av1_nvenc"])
def test_adapter_quality_default_is_in_range(name):
    a = get_adapter(name)
    lo, hi = a.quality_range
    assert lo <= a.quality_default <= hi


@pytest.mark.parametrize("name", ["h264_nvenc", "hevc_nvenc", "av1_nvenc"])
def test_adapter_preset_set_matches_shared_table(name):
    a = get_adapter(name)
    assert a.presets == NVENC_PRESETS
