# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Repeated encoder-parameter options merge into one (RC4 WP15).

FFmpeg keeps only the last occurrence of ``-x265-params`` (and of the x264,
SVT-AV1 and VVenC equivalents). A two-pass adapter, a saliency augment and an
HDR block each add their own, so the argv builder joins them. Mirror of
``ffencode.MergeCodecParams`` (``pkg/ffencode``).
"""

from __future__ import annotations

import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.encode import EncodeRequest, build_ffmpeg_command, merge_codec_params


def _x265_request(extra: tuple[str, ...]) -> EncodeRequest:
    return EncodeRequest(
        source=Path("/src/ref.yuv"),
        width=1920,
        height=1080,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder="libx265",
        preset="medium",
        crf=23,
        output=Path("/out/enc.mp4"),
        extra_params=extra,
        pass_number=2,
        stats_path=Path("/tmp/stats"),
    )


def test_two_pass_saliency_and_hdr_share_one_x265_params():
    req = _x265_request(
        (
            "-x265-params",
            "zones=0,9,q=-4",
            "-color_primaries",
            "bt2020",
            "-x265-params",
            "master-display=G(1,2)B(3,4)R(5,6)WP(7,8)L(9,1)",
        )
    )
    argv = build_ffmpeg_command(req)
    assert argv.count("-x265-params") == 1, argv
    value = argv[argv.index("-x265-params") + 1]
    for want in ("pass=2:stats=/tmp/stats", "zones=0,9,q=-4", "master-display=G(1,2)"):
        assert want in value
    assert "-color_primaries" in argv  # an option between the two is kept


def test_merge_codec_params_cases():
    assert merge_codec_params(["-c:v", "libx264", "o.mp4"]) == ["-c:v", "libx264", "o.mp4"]
    assert merge_codec_params(["-x264-params", "a=1", "o"]) == ["-x264-params", "a=1", "o"]
    assert merge_codec_params(
        ["-x265-params", "a=1", "-b:v", "2k", "-x265-params", "b=2", "o"]
    ) == [
        "-x265-params",
        "a=1:b=2",
        "-b:v",
        "2k",
        "o",
    ]
    assert merge_codec_params(
        ["-x265-params", "a=1", "-svtav1-params", "b=2", "-x265-params", "c=3"]
    ) == [
        "-x265-params",
        "a=1:c=3",
        "-svtav1-params",
        "b=2",
    ]
    assert merge_codec_params(["-x265-params", "a=1", "-x265-params", ""]) == [
        "-x265-params",
        "a=1",
    ]
    assert merge_codec_params(["-x265-params"]) == ["-x265-params"]


def test_saliency_was_applied_keys_on_the_roi_keys_not_the_flag():
    from vmaftune.encode import EncodeResult
    from vmaftune.executor import _saliency_was_applied

    def result(extra: tuple[str, ...]) -> EncodeResult:
        req = _x265_request(extra)
        return EncodeResult(
            request=req,
            encode_size_bytes=1,
            encode_time_ms=1.0,
            encoder_version="x",
            ffmpeg_version="y",
            exit_status=0,
            stderr_tail="",
        )

    hdr_only = result(("-x265-params", "master-display=G(1,2)B(3,4)R(5,6)WP(7,8)L(9,1)"))
    zones = result(("-x265-params", "master-display=G(1,2):zones=0,9,q=-4"))
    qpfile = result(("-qpfile", "/tmp/q.txt"))
    assert not _saliency_was_applied(hdr_only.request, hdr_only)
    assert _saliency_was_applied(zones.request, zones)
    assert _saliency_was_applied(qpfile.request, qpfile)
