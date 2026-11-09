# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""libx265 two-pass cell at a CRF: pass 1 at the CRF, pass 2 ABR at its bitrate.

ADR-1565 (state row ``T-VMAFTUNE-X265-TWO-PASS-CRF-2026-10-04``). x265 exits
183 on a pass 2 that keeps ``-crf``; the cell now measures pass 1's bitstream
and encodes pass 2 at that bitrate. The mocked cases are hermetic; the real
case runs ffmpeg with libx265 on a 2 s synthetic clip.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

import pytest

FFMPEG_TIMEOUT_S = 60

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "src"))

from vmaftune.codec_adapters import get_adapter
from vmaftune.encode import (
    EncodeRequest,
    _with_abr_rate_control,
    build_ffmpeg_command,
    run_two_pass_encode,
)


class _Done:
    def __init__(self, returncode: int, stdout: str = "", stderr: str = "") -> None:
        self.returncode, self.stdout, self.stderr = returncode, stdout, stderr


def _req(tmp_path: Path, encoder: str = "libx265", **kw) -> EncodeRequest:
    src = tmp_path / "ref.yuv"
    src.write_bytes(b"\x80" * 1024)
    return EncodeRequest(
        source=src,
        width=64,
        height=64,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder=encoder,
        preset="medium",
        crf=28,
        output=tmp_path / "out.mp4",
        **kw,
    )


def test_adapter_declares_abr_at_pass1_bitrate_and_a_new_cache_version() -> None:
    adapter = get_adapter("libx265")
    assert adapter.two_pass_abr_at_pass1_bitrate is True
    assert adapter.adapter_version == "2", "cached 1-pass-keyed results must not alias"
    assert getattr(get_adapter("libx264"), "two_pass_abr_at_pass1_bitrate", False) is False


def test_abr_request_swaps_crf_for_bitrate(tmp_path: Path) -> None:
    req = _req(tmp_path, pass_number=2, stats_path=tmp_path / "s.stats", abr_bitrate_kbps=1234.4)
    cmd = build_ffmpeg_command(req)
    assert "-crf" not in cmd
    assert cmd[cmd.index("-b:v") + 1] == "1234k"
    assert cmd.count("-b:v") == 1
    assert "pass=2:stats=" in cmd[cmd.index("-x265-params") + 1]


def test_without_abr_the_argv_is_unchanged(tmp_path: Path) -> None:
    req = _req(tmp_path, pass_number=2, stats_path=tmp_path / "s.stats")
    cmd = build_ffmpeg_command(req)
    assert cmd[cmd.index("-crf") + 1] == "28" and "-b:v" not in cmd


def test_abr_without_a_crf_to_replace_is_an_error(tmp_path: Path) -> None:
    req = _req(tmp_path, abr_bitrate_kbps=500.0)
    with pytest.raises(ValueError, match="no -crf to replace"):
        _with_abr_rate_control(["-c:v", "libx265", "-b:v", "1k"], req)


def test_pass1_output_replaces_the_null_muxer(tmp_path: Path) -> None:
    out1 = tmp_path / "p1.mp4"
    req = _req(tmp_path, pass_number=1, stats_path=tmp_path / "s.stats", pass1_output=out1)
    assert build_ffmpeg_command(req)[-1] == str(out1)
    plain = _req(tmp_path, pass_number=1, stats_path=tmp_path / "s.stats")
    assert build_ffmpeg_command(plain)[-3:] == ["-f", "null", "-"]


def test_missing_pass1_bitrate_fails_the_cell_without_running_pass2(tmp_path: Path) -> None:
    calls: list[list[str]] = []

    def runner(cmd, capture_output, text, check):
        calls.append(list(cmd))
        if cmd[0] == "ffprobe":
            return _Done(0, stdout="N/A\n")
        Path(cmd[-1]).write_bytes(b"\x00" * 10)
        return _Done(0, stderr="ffmpeg version 6.1.1\nx265 [info]: HEVC encoder version 3.5+1\n")

    res = run_two_pass_encode(_req(tmp_path), runner=runner)
    assert res.exit_status == 1
    assert "pass 1 bitrate unavailable" in res.stderr_tail
    assert [c[0] for c in calls] == ["ffmpeg", "ffprobe"], "pass 2 must not run"


def test_ffprobe_failure_fails_the_cell(tmp_path: Path) -> None:
    def runner(cmd, capture_output, text, check):
        if cmd[0] == "ffprobe":
            return _Done(1)
        Path(cmd[-1]).write_bytes(b"\x00")
        return _Done(0, stderr="ffmpeg version 6.1.1\nx265 [info]: HEVC encoder version 3.5+1\n")

    res = run_two_pass_encode(_req(tmp_path), runner=runner)
    assert res.exit_status == 1 and "pass 1 bitrate unavailable" in res.stderr_tail


def test_pass1_failure_skips_the_rest(tmp_path: Path) -> None:
    calls: list[str] = []

    def runner(cmd, capture_output, text, check):
        calls.append(cmd[0])
        return _Done(1, stderr="boom")

    res = run_two_pass_encode(_req(tmp_path), runner=runner)
    assert res.exit_status == 1 and res.stderr_tail.startswith("[pass 1 failed]")
    assert calls == ["ffmpeg"]


def test_ffprobe_sits_next_to_a_custom_ffmpeg(tmp_path: Path) -> None:
    seen: list[str] = []

    def runner(cmd, capture_output, text, check):
        seen.append(cmd[0])
        if "ffprobe" in cmd[0]:
            return _Done(0, stdout="100000\n")
        Path(cmd[-1]).write_bytes(b"\x00" * 5)
        return _Done(0, stderr="ffmpeg version 6.1.1\nx265 [info]: HEVC encoder version 3.5+1\n")

    run_two_pass_encode(_req(tmp_path), runner=runner, ffmpeg_bin="/opt/x/bin/ffmpeg")
    assert seen == ["/opt/x/bin/ffmpeg", "/opt/x/bin/ffprobe", "/opt/x/bin/ffmpeg"]


def test_corpus_row_records_the_abr_rate_control(tmp_path: Path) -> None:
    """The row's ``extra_params`` lists ``-b:v``; ``crf`` stays the pass-1 CRF."""
    from types import SimpleNamespace

    from vmaftune.corpus import _row_result_columns

    def runner(cmd, capture_output, text, check):
        if cmd[0] == "ffprobe":
            return _Done(0, stdout="2500000\n")
        Path(cmd[-1]).write_bytes(b"\x00" * 100)
        return _Done(0, stderr="ffmpeg version 6.1.1\nx265 [info]: HEVC encoder version 3.5+1\n")

    enc = run_two_pass_encode(_req(tmp_path), runner=runner)
    score = SimpleNamespace(
        vmaf_score=93.0, score_time_ms=1.0, vmaf_binary_version="x", exit_status=0
    )
    cols = _row_result_columns(
        job=SimpleNamespace(duration_s=1.0),
        opts=SimpleNamespace(encoder="libx265", keep_encodes=False),
        preset="medium",
        crf=28,
        enc_res=enc,
        score_res=score,
        score_model="m",
        clip_mode="full",
    )
    assert cols["crf"] == 28
    assert cols["extra_params"][-2:] == ["-b:v", "2500k"]


@pytest.mark.skipif(
    shutil.which("ffmpeg") is None or shutil.which("ffprobe") is None,
    reason="needs ffmpeg and ffprobe on PATH",
)
def test_real_x265_two_pass_at_a_crf_encodes(tmp_path: Path) -> None:
    enc = subprocess.run(
        ["ffmpeg", "-hide_banner", "-encoders"],
        capture_output=True,
        text=True,
        check=False,
        timeout=FFMPEG_TIMEOUT_S,
    )
    if "libx265" not in enc.stdout:
        pytest.skip("this ffmpeg build has no libx265 encoder")
    yuv = tmp_path / "src.yuv"
    gen = subprocess.run(
        [
            "ffmpeg",
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-f",
            "lavfi",
            "-i",
            "testsrc2=size=320x180:rate=24",
            "-t",
            "2",
            "-pix_fmt",
            "yuv420p",
            str(yuv),
        ],
        capture_output=True,
        check=False,
        timeout=FFMPEG_TIMEOUT_S,
    )
    assert gen.returncode == 0
    req = EncodeRequest(
        source=yuv,
        width=320,
        height=180,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder="libx265",
        preset="ultrafast",
        crf=28,
        output=tmp_path / "out.mp4",
    )
    res = run_two_pass_encode(req)
    assert res.exit_status == 0, res.stderr_tail
    assert res.encode_size_bytes > 0
    assert res.request.extra_params[-2] == "-b:v" and res.request.extra_params[-1].endswith("k")
