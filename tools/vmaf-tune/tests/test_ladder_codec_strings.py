# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""RFC 6381 codec strings of ladder renditions.

The HLS and DASH writers printed ``avc1.640028`` (H.264 High, level 4.0)
for every rung of every encoder. The string now comes from a two-frame
encode of the rendition (:mod:`vmaftune.codec_strings`), so it names the
codec, profile and level the encoder actually uses.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune import cli
from vmaftune.codec_strings import (
    CodecStringError,
    codec_string_from_mp4,
    probe_argv,
    probe_codec_string,
    vp9_level,
)
from vmaftune.ladder import Rendition, emit_manifest


def _box(fourcc: bytes, payload: bytes) -> bytes:
    return (8 + len(payload)).to_bytes(4, "big") + fourcc + payload


def _mp4(*boxes: bytes) -> bytes:
    return _box(b"ftyp", b"isom\x00\x00\x02\x00") + b"".join(boxes) + _box(b"mdat", b"\x00" * 16)


# Configuration records as libx264 / libx265 / libsvtav1 / libvpx-vp9 write
# them (bytes taken from real encodes on 2026-10-04).
AVC_HIGH_40 = bytes.fromhex("0164002 8ffe1".replace(" ", ""))
AVC_CBP_30 = bytes.fromhex("0142c01effe1")
HEVC_MAIN_L120 = bytes.fromhex("01016000000090000000000078f000fc")
AV1_MAIN_8BIT_L12 = bytes.fromhex("810c0c00")
AV1_PROF2_12BIT_HIGH_TIER = bytes([0x81, (2 << 5) | 13, 0b1110_0000, 0])
VP9_PROFILE0_8BIT = bytes.fromhex("010000000028820202020000")


@pytest.mark.parametrize(
    ("fourcc", "record", "want"),
    [
        (b"avcC", AVC_HIGH_40, "avc1.640028"),
        (b"avcC", AVC_CBP_30, "avc1.42c01e"),
        (b"hvcC", HEVC_MAIN_L120, "hvc1.1.6.L120.90"),
        (b"av1C", AV1_MAIN_8BIT_L12, "av01.0.12M.08"),
        (b"av1C", AV1_PROF2_12BIT_HIGH_TIER, "av01.2.13H.12"),
        (b"vpcC", VP9_PROFILE0_8BIT, "vp09.00.40.08"),
    ],
)
def test_codec_string_from_config_record(fourcc, record, want):
    data = _mp4(_box(b"moov", _box(fourcc, record)))
    assert codec_string_from_mp4(data, width=1920, height=1080, framerate=30.0) == want


def test_hevc_profile_space_tier_and_trailing_zero_constraint_bytes():
    # profile_space 1 (A), high tier, profile 2 (Main 10), compat 0x20000000
    # (reversed: 0x4), constraint bytes B0 then zeros (dropped), level 153.
    record = bytes([0x01, (1 << 6) | (1 << 5) | 2]) + bytes.fromhex("20000000")
    record += bytes.fromhex("B00000000000") + bytes([153]) + b"\x00\x00\x00"
    data = _mp4(_box(b"hvcC", record))
    assert codec_string_from_mp4(data, width=3840, height=2160, framerate=60.0) == (
        "hvc1.A2.4.H153.B0"
    )


def test_no_supported_record_is_an_error():
    """VVC, ProRes and anything else without these records refuse, never guess."""
    with pytest.raises(CodecStringError, match="H.264, HEVC, AV1 and VP9"):
        codec_string_from_mp4(_mp4(_box(b"vvcC", b"\x00" * 8)), width=1, height=1, framerate=1.0)


@pytest.mark.parametrize(
    ("width", "height", "fps", "kbps", "level"),
    [
        (256, 144, 15.0, 100.0, 10),
        (640, 360, 24.0, 1000.0, 21),
        (1920, 1080, 30.0, 5000.0, 40),
        (1920, 1080, 60.0, 5000.0, 41),
        (1920, 1080, 30.0, 25_000.0, 41),  # bitrate above level 4's 18 Mbit/s
        (3840, 2160, 60.0, 20_000.0, 51),
        (8192, 4352, 120.0, 400_000.0, 62),
    ],
)
def test_vp9_level_table(width, height, fps, kbps, level):
    assert vp9_level(width, height, fps, kbps) == level


def test_vp9_level_beyond_the_table_is_an_error():
    with pytest.raises(CodecStringError, match="no VP9 level"):
        vp9_level(16_384, 8_704, 240.0)


def _ladder() -> list[Rendition]:
    return [
        Rendition(width=640, height=360, bitrate_kbps=800.0, vmaf=85.0, crf=30),
        Rendition(width=1920, height=1080, bitrate_kbps=5000.0, vmaf=95.0, crf=24),
    ]


def test_writers_name_each_rendition_with_its_own_string():
    def codec_for(r: Rendition) -> str:
        return {640: "hvc1.1.6.L63.90", 1920: "hvc1.1.6.L120.90"}[r.width]

    hls = emit_manifest(_ladder(), "hls", codec_for=codec_for)
    assert 'RESOLUTION=640x360,CODECS="hvc1.1.6.L63.90"' in hls
    assert 'RESOLUTION=1920x1080,CODECS="hvc1.1.6.L120.90"' in hls
    dash = emit_manifest(_ladder(), "dash", codec_for=codec_for)
    assert 'codecs="hvc1.1.6.L120.90"' in dash
    assert "avc1" not in hls + dash


def test_writers_without_a_resolver_name_no_codec():
    """The constant avc1.640028 named the wrong codec for every non-H.264 ladder."""
    hls = emit_manifest(_ladder(), "hls")
    dash = emit_manifest(_ladder(), "dash")
    assert "CODECS" not in hls and "avc1" not in hls
    assert "codecs=" not in dash and "avc1" not in dash


def test_probe_argv_uses_the_adapter_argv_and_the_qsv_chain(tmp_path):
    out = tmp_path / "p.mp4"
    x264 = probe_argv("libx264", "medium", 23, 1280, 720, 29.97, "yuv420p10le", out)
    assert "testsrc2=size=1280x720:rate=29.97,format=yuv420p10le" in x264
    assert x264[x264.index("-c:v") : x264.index("-c:v") + 6] == [
        "-c:v",
        "libx264",
        "-preset",
        "medium",
        "-crf",
        "23",
    ]
    assert x264[-1] == str(out) and "+faststart" in x264
    qsv = probe_argv("h264_qsv", "medium", 23, 1280, 720, 24.0, "yuv420p", out, "ffmpeg")
    assert qsv.index("-init_hw_device") < qsv.index("-i")
    assert "format=nv12,hwupload=extra_hw_frames=64" in qsv


@pytest.mark.parametrize("pix_fmt", ["p010le", "yuv420p10le"])
def test_qsv_codec_string_probe_preserves_10bit_depth(tmp_path, pix_fmt):
    argv = probe_argv("hevc_qsv", "medium", 23, 1280, 720, 24.0, pix_fmt, tmp_path / "p.mp4")
    assert argv[argv.index("-vf") + 1] == "format=p010le,hwupload=extra_hw_frames=64"


def test_probe_failure_is_an_error(tmp_path):
    class _Done:
        returncode, stdout, stderr = 1, "", "Unknown encoder 'h264_amf'\n"

    with pytest.raises(CodecStringError, match="Unknown encoder"):
        probe_codec_string(
            "h264_amf",
            preset="medium",
            quality=23,
            width=640,
            height=360,
            framerate=24.0,
            runner=lambda *a, **k: _Done(),
        )


def _ffmpeg_has(encoder: str) -> bool:
    if shutil.which("ffmpeg") is None:
        return False
    out = subprocess.run(
        ["ffmpeg", "-hide_banner", "-encoders"], capture_output=True, text=True, check=False
    ).stdout
    return any(line.split()[1:2] == [encoder] for line in out.splitlines())


@pytest.mark.skipif(not _ffmpeg_has("libx264"), reason="needs an ffmpeg with libx264")
def test_real_probe_names_the_level_of_each_geometry():
    """A real two-frame encode: 360p24 is level 3.0, 1080p30 level 4.0, 2160p60 level 5.2."""
    got = [
        probe_codec_string("libx264", preset="medium", quality=23, width=w, height=h, framerate=f)
        for w, h, f in ((640, 360, 24.0), (1920, 1080, 30.0), (3840, 2160, 60.0))
    ]
    assert got == ["avc1.64001e", "avc1.640028", "avc1.640034"]


def test_ladder_cli_resolves_codecs_for_hls_and_dash(tmp_path, monkeypatch):
    calls: list[tuple] = []

    def fake_probe(encoder, **kw):
        calls.append((encoder, kw["width"], kw["height"], kw["framerate"], kw["pix_fmt"]))
        return "hvc1.1.6.L93.90"

    monkeypatch.setattr("vmaftune.codec_strings.probe_codec_string", fake_probe)
    args = cli._build_parser().parse_args(
        [
            "ladder",
            "--src",
            str(tmp_path / "ref.yuv"),
            "--encoder",
            "libx265",
            "--resolutions",
            "1280x720",
            "--target-vmafs",
            "90",
            "--framerate",
            "25",
        ]
    )
    resolve = cli._ladder_codec_resolver(args)
    rung = Rendition(width=1280, height=720, bitrate_kbps=2000.0, vmaf=90.0, crf=28)
    assert resolve(rung) == "hvc1.1.6.L93.90"
    assert calls == [("libx265", 1280, 720, 25.0, "yuv420p")]
