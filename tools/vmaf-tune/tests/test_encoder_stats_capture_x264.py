# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Capture-flow tests for ``run_encode_with_stats`` (ADR-0332).

Mocks ``subprocess.run`` and drops a canned stats file at the path
FFmpeg would have written. Asserts the resulting :class:`EncodeResult`
carries the parsed per-frame records.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.encode import (
    EncodeRequest,
    build_pass1_stats_command,
    run_encode_with_stats,
)


class _FakeCompleted:
    def __init__(self, returncode: int = 0, stderr: str = ""):
        self.returncode = returncode
        self.stdout = ""
        self.stderr = stderr


_X264_STATS_FIXTURE = (
    "#options: 64x64 fps=24/1 timebase=1/24 bitdepth=8 cabac=1 ref=1 rc=crf "
    "mbtree=1 crf=23.0\n"
    "in:0 out:0 type:I dur:1 cpbdur:1 q:23.00 aq:20.00 tex:5000 mv:0 misc:200 "
    "imb:16 pmb:0 smb:0 d:- ref:;\n"
    "in:1 out:1 type:P dur:1 cpbdur:1 q:23.00 aq:21.00 tex:1500 mv:50 misc:80 "
    "imb:1 pmb:10 smb:5 d:- ref:0 ;\n"
    "in:2 out:2 type:b dur:1 cpbdur:1 q:23.00 aq:18.00 tex:120 mv:5 misc:40 "
    "imb:0 pmb:2 smb:14 d:- ref:0 ;\n"
)


def _make_request(tmp_path: Path) -> EncodeRequest:
    src = tmp_path / "ref.yuv"
    src.write_bytes(b"\x80" * 4096)
    return EncodeRequest(
        source=src,
        width=64,
        height=64,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder="libx264",
        preset="medium",
        crf=23,
        output=tmp_path / "encodes" / "out.mp4",
    )


def test_build_pass1_stats_command_targets_devnull(tmp_path: Path):
    req = _make_request(tmp_path)
    prefix = tmp_path / "stats_prefix"
    cmd = build_pass1_stats_command(req, prefix, ffmpeg_bin="ffmpeg")
    # Pass-1 + passlogfile + null muxer must all be present.
    assert "-pass" in cmd
    assert cmd[cmd.index("-pass") + 1] == "1"
    assert "-passlogfile" in cmd
    assert cmd[cmd.index("-passlogfile") + 1] == str(prefix)
    # Output is ``-f null <devnull>`` at the tail of the argv.
    assert cmd[-3] == "-f"
    assert cmd[-2] == "null"
    assert cmd[-1] in ("/dev/null", "nul")


def test_run_encode_with_stats_attaches_parsed_frames(tmp_path: Path):
    req = _make_request(tmp_path)
    req.output.parent.mkdir(parents=True, exist_ok=True)

    stats_dir = tmp_path / "stats"
    stats_dir.mkdir()

    calls: list[list[str]] = []

    def fake_run(cmd, capture_output, text, check):
        calls.append(list(cmd))
        # Pass-1 invocation: drop the canned stats file at the
        # path the wrapper expects (``<prefix>-0.log``).
        if "-pass" in cmd and "1" == cmd[cmd.index("-pass") + 1]:
            prefix = Path(cmd[cmd.index("-passlogfile") + 1])
            log = prefix.parent / f"{prefix.name}-0.log"
            log.write_text(_X264_STATS_FIXTURE)
            return _FakeCompleted(returncode=0)
        # Main encode: produce the bitstream proxy.
        out_path = Path(cmd[-1])
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_bytes(b"\x00" * 1024)
        return _FakeCompleted(
            returncode=0,
            stderr="ffmpeg version 6.1.1\nx264 - core 164 r3107\n",
        )

    result = run_encode_with_stats(
        req,
        ffmpeg_bin="ffmpeg",
        runner=fake_run,
        stats_dir=stats_dir,
    )

    # Both invocations happened.
    assert len(calls) == 2
    assert "-pass" in calls[0]
    assert "-pass" not in calls[1]

    # Stats parsed and attached.
    assert len(result.encoder_stats) == 3
    assert result.encoder_stats[0].frame_type == "I"
    assert result.encoder_stats[1].frame_type == "P"
    assert result.encoder_stats[2].frame_type == "b"
    # Sanity: tex from the fixture's I-frame survives the round-trip.
    assert result.encoder_stats[0].tex == 5000

    # Main-encode result fields preserved.
    assert result.exit_status == 0
    assert result.encode_size_bytes == 1024


def test_run_encode_with_stats_capture_disabled_short_circuits(tmp_path: Path):
    req = _make_request(tmp_path)
    req.output.parent.mkdir(parents=True, exist_ok=True)

    calls: list[list[str]] = []

    def fake_run(cmd, capture_output, text, check):
        calls.append(list(cmd))
        out_path = Path(cmd[-1])
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_bytes(b"\x00" * 256)
        return _FakeCompleted(returncode=0, stderr="x264 - core 164\n")

    result = run_encode_with_stats(
        req,
        runner=fake_run,
        capture_stats=False,
    )
    assert len(calls) == 1
    assert result.encoder_stats == ()


def test_run_encode_with_stats_tolerates_missing_stats_file(tmp_path: Path):
    """Pass-1 ran but didn't write a stats file (e.g. encoder crashed).

    The wrapper must still return a valid EncodeResult — just with an
    empty ``encoder_stats`` tuple — so the corpus row stays uniform.
    """
    req = _make_request(tmp_path)
    req.output.parent.mkdir(parents=True, exist_ok=True)

    def fake_run(cmd, capture_output, text, check):
        if "-pass" not in cmd:
            out_path = Path(cmd[-1])
            out_path.parent.mkdir(parents=True, exist_ok=True)
            out_path.write_bytes(b"\x00" * 128)
        return _FakeCompleted(returncode=0, stderr="x264 - core 164\n")

    result = run_encode_with_stats(
        req,
        runner=fake_run,
        stats_dir=tmp_path / "stats",
    )
    assert result.encoder_stats == ()
    assert result.exit_status == 0


def test_pass1_command_carries_sample_clip_flags(tmp_path: Path):
    src = tmp_path / "ref.yuv"
    src.write_bytes(b"\x80" * 1024)
    req = EncodeRequest(
        source=src,
        width=64,
        height=64,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder="libx264",
        preset="medium",
        crf=23,
        output=tmp_path / "out.mp4",
        sample_clip_seconds=4.0,
        sample_clip_start_s=2.0,
    )
    cmd = build_pass1_stats_command(req, tmp_path / "p")
    i_pos = cmd.index("-i")
    ss_pos = cmd.index("-ss")
    t_pos = cmd.index("-t")
    assert ss_pos < i_pos
    assert t_pos < i_pos
    assert cmd[ss_pos + 1] == "2.0"
    assert cmd[t_pos + 1] == "4.0"


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
