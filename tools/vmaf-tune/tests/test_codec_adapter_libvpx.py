# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for the libvpx-vp9 codec adapter."""

from __future__ import annotations

import dataclasses
import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.codec_adapters import (
    LibvpxVp9Adapter,
    get_adapter,
    known_codecs,
)
from vmaftune.encode import (
    EncodeRequest,
    build_ffmpeg_command,
    parse_versions,
)


def test_libvpx_vp9_registered():
    assert "libvpx-vp9" in known_codecs()
    adapter = get_adapter("libvpx-vp9")
    assert isinstance(adapter, LibvpxVp9Adapter)
    assert adapter.encoder == "libvpx-vp9"
    assert adapter.quality_knob == "crf"
    assert adapter.supports_two_pass is True
    assert adapter.supports_encoder_stats is False


def test_libvpx_vp9_preset_mapping():
    adapter = LibvpxVp9Adapter()
    expected = {
        "placebo": 0,
        "slowest": 0,
        "slower": 1,
        "slow": 2,
        "medium": 3,
        "fast": 4,
        "faster": 5,
        "veryfast": 5,
        "superfast": 5,
        "ultrafast": 5,
    }
    assert set(adapter.presets) == set(expected)
    for preset, cpu_used in expected.items():
        assert adapter.cpu_used(preset) == cpu_used


def test_libvpx_vp9_validation():
    adapter = LibvpxVp9Adapter()
    adapter.validate("medium", 0)
    adapter.validate("medium", 63)
    with pytest.raises(ValueError, match="unknown libvpx-vp9 preset"):
        adapter.validate("turbo", 32)
    with pytest.raises(ValueError, match="outside libvpx-vp9 range"):
        adapter.validate("medium", -1)
    with pytest.raises(ValueError, match="outside libvpx-vp9 range"):
        adapter.validate("medium", 64)


def test_libvpx_vp9_ffmpeg_args_shape():
    adapter = LibvpxVp9Adapter()
    assert adapter.ffmpeg_codec_args("medium", 32) == [
        "-c:v",
        "libvpx-vp9",
        "-deadline",
        "good",
        "-cpu-used",
        "3",
        "-crf",
        "32",
        "-b:v",
        "0",
    ]
    assert adapter.extra_params() == ("-row-mt", "1")


def test_libvpx_vp9_build_ffmpeg_command_routes_adapter():
    req = EncodeRequest(
        source=Path("ref.yuv"),
        width=1920,
        height=1080,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder="libvpx-vp9",
        preset="fast",
        crf=30,
        output=Path("out.webm"),
    )
    cmd = build_ffmpeg_command(req, ffmpeg_bin="ffmpeg")
    assert cmd[0] == "ffmpeg"
    assert cmd[cmd.index("-c:v") + 1] == "libvpx-vp9"
    assert cmd[cmd.index("-deadline") + 1] == "good"
    assert cmd[cmd.index("-cpu-used") + 1] == "4"
    assert cmd[cmd.index("-crf") + 1] == "30"
    assert cmd[cmd.index("-b:v") + 1] == "0"
    assert cmd[cmd.index("-row-mt") + 1] == "1"
    assert cmd[-1] == "out.webm"


def test_libvpx_vp9_two_pass_args():
    adapter = LibvpxVp9Adapter()
    stats = Path("/tmp/vmaf-vpx")
    assert adapter.two_pass_args(0, stats) == ()
    assert adapter.two_pass_args(1, stats) == ("-pass", "1", "-passlogfile", str(stats))
    assert adapter.two_pass_args(2, stats) == ("-pass", "2", "-passlogfile", str(stats))
    with pytest.raises(ValueError, match="pass_number must be 1 or 2"):
        adapter.two_pass_args(3, stats)


def test_libvpx_vp9_build_ffmpeg_command_routes_two_pass():
    req = EncodeRequest(
        source=Path("ref.yuv"),
        width=64,
        height=64,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder="libvpx-vp9",
        preset="medium",
        crf=32,
        output=Path("out.webm"),
        pass_number=1,
        stats_path=Path("/tmp/vmaf-vpx"),
    )
    cmd = build_ffmpeg_command(req, ffmpeg_bin="ffmpeg")
    assert cmd[cmd.index("-pass") + 1] == "1"
    assert cmd[cmd.index("-passlogfile") + 1] == "/tmp/vmaf-vpx"
    assert cmd[-3:] == ["-f", "null", "-"]


def test_libvpx_vp9_parse_versions():
    stderr = "ffmpeg version 7.0\n[libvpx-vp9 @ 0xabc] v1.13.1\n"
    assert parse_versions(stderr, encoder="libvpx-vp9") == ("7.0", "libvpx-vp9-1.13.1")
    assert parse_versions("ffmpeg version 7.0\n", encoder="libvpx-vp9") == (
        "7.0",
        "unknown",
    )


def test_libvpx_vp9_adapter_is_frozen_dataclass():
    adapter = LibvpxVp9Adapter()
    with pytest.raises(dataclasses.FrozenInstanceError):
        adapter.name = "other"  # type: ignore[misc]
