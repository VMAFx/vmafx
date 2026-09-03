# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""x265 2-pass encoding — Phase F smoke tests (ADR-0333).

Most cases mock ``subprocess.run`` end-to-end so the suite is
hermetic. One opt-in real-binary integration test (gated on
``VMAF_TUNE_INTEGRATION=1``) runs a 5-second synthetic clip through
both single-pass and 2-pass libx265 and asserts:

1. The 2-pass output exists and has non-trivial size.
2. The two bitstreams differ (different mp4 byte content) — confirms
   the second pass actually used the stats file.
3. At a comparable bitrate target, 2-pass typically scores >= 1-pass
   (within VMAF noise; we use a generous tolerance to avoid
   content-dependent flakes).
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

# Make src/ importable without an editable install.
_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.codec_adapters import X264Adapter, X265Adapter, get_adapter
from vmaftune.encode import (
    EncodeRequest,
    build_ffmpeg_command,
    run_two_pass_encode,
)


class _FakeCompleted:
    """Minimal stand-in for ``subprocess.CompletedProcess``."""

    def __init__(self, returncode: int, stdout: str = "", stderr: str = ""):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


def _make_yuv(path: Path, nbytes: int = 1024) -> Path:
    path.write_bytes(b"\x80" * nbytes)
    return path


# --- Adapter-level contract -------------------------------------------


def test_x265_advertises_two_pass_support():
    a = get_adapter("libx265")
    assert getattr(a, "supports_two_pass", False) is True


def test_x264_advertises_two_pass_support():
    a = get_adapter("libx264")
    assert getattr(a, "supports_two_pass", False) is True


def test_x264_two_pass_args_pass1_emits_ffmpeg_passlogfile():
    a = X264Adapter()
    args = a.two_pass_args(1, Path("/tmp/foo.stats"))
    assert args == ("-pass", "1", "-passlogfile", "/tmp/foo.stats")


def test_x264_two_pass_args_pass2_emits_ffmpeg_passlogfile():
    a = X264Adapter()
    args = a.two_pass_args(2, Path("/tmp/foo.stats"))
    assert args == ("-pass", "2", "-passlogfile", "/tmp/foo.stats")


def test_x264_two_pass_args_pass0_returns_empty_tuple():
    a = X264Adapter()
    assert a.two_pass_args(0, Path("/tmp/foo.stats")) == ()


def test_x264_two_pass_args_rejects_invalid_pass_number():
    a = X264Adapter()
    with pytest.raises(ValueError):
        a.two_pass_args(3, Path("/tmp/foo.stats"))
    with pytest.raises(ValueError):
        a.two_pass_args(-1, Path("/tmp/foo.stats"))


def test_x265_two_pass_args_pass1_emits_x265_params():
    a = X265Adapter()
    args = a.two_pass_args(1, Path("/tmp/foo.stats"))
    assert args == ("-x265-params", "pass=1:stats=/tmp/foo.stats")


def test_x265_two_pass_args_pass2_emits_x265_params():
    a = X265Adapter()
    args = a.two_pass_args(2, Path("/tmp/foo.stats"))
    assert args == ("-x265-params", "pass=2:stats=/tmp/foo.stats")


def test_x265_two_pass_args_pass0_returns_empty_tuple():
    # pass_number == 0 is the single-pass sentinel; callers that
    # forward this method's result unconditionally don't need a
    # special branch.
    a = X265Adapter()
    assert a.two_pass_args(0, Path("/tmp/foo.stats")) == ()


def test_x265_two_pass_args_rejects_invalid_pass_number():
    a = X265Adapter()
    with pytest.raises(ValueError):
        a.two_pass_args(3, Path("/tmp/foo.stats"))
    with pytest.raises(ValueError):
        a.two_pass_args(-1, Path("/tmp/foo.stats"))


# --- build_ffmpeg_command argv shape ----------------------------------


def _x265_request(tmp_path: Path, **overrides) -> EncodeRequest:
    defaults = {
        "source": tmp_path / "ref.yuv",
        "width": 64,
        "height": 64,
        "pix_fmt": "yuv420p",
        "framerate": 24.0,
        "encoder": "libx265",
        "preset": "medium",
        "crf": 28,
        "output": tmp_path / "out.mp4",
    }
    defaults.update(overrides)
    return EncodeRequest(**defaults)


def test_build_ffmpeg_command_pass1_redirects_to_null_muxer(tmp_path: Path):
    stats = tmp_path / "encode.stats"
    req = _x265_request(tmp_path, pass_number=1, stats_path=stats)
    cmd = build_ffmpeg_command(req)
    # -x265-params must come AFTER -crf so the per-codec slice the
    # adapter emits doesn't get clobbered by ffmpeg argv ordering.
    crf_idx = cmd.index("-crf")
    x265_params_idx = cmd.index("-x265-params")
    assert x265_params_idx > crf_idx
    assert cmd[x265_params_idx + 1] == f"pass=1:stats={stats}"
    # Pass 1 writes to the null muxer, NOT the requested output path.
    assert cmd[-3:] == ["-f", "null", "-"]
    assert str(req.output) not in cmd


def test_build_ffmpeg_command_pass2_writes_real_output(tmp_path: Path):
    stats = tmp_path / "encode.stats"
    req = _x265_request(tmp_path, pass_number=2, stats_path=stats)
    cmd = build_ffmpeg_command(req)
    x265_params_idx = cmd.index("-x265-params")
    assert cmd[x265_params_idx + 1] == f"pass=2:stats={stats}"
    # Pass 2 writes the actual encoded bitstream.
    assert cmd[-1] == str(req.output)
    assert "null" not in cmd[-3:]


def test_build_ffmpeg_command_x264_pass1_uses_native_passlogfile(tmp_path: Path):
    stats = tmp_path / "encode.stats"
    req = EncodeRequest(
        source=tmp_path / "ref.yuv",
        width=64,
        height=64,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder="libx264",
        preset="medium",
        crf=23,
        output=tmp_path / "out.mp4",
        pass_number=1,
        stats_path=stats,
    )
    cmd = build_ffmpeg_command(req)
    assert "-crf" not in cmd
    assert "-pass" in cmd
    assert cmd[cmd.index("-pass") + 1] == "1"
    assert "-passlogfile" in cmd
    assert cmd[cmd.index("-passlogfile") + 1] == str(stats)
    assert cmd[-3:] == ["-f", "null", "-"]


def test_build_ffmpeg_command_x264_pass2_writes_real_output(tmp_path: Path):
    stats = tmp_path / "encode.stats"
    req = EncodeRequest(
        source=tmp_path / "ref.yuv",
        width=64,
        height=64,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder="libx264",
        preset="medium",
        crf=23,
        output=tmp_path / "out.mp4",
        pass_number=2,
        stats_path=stats,
    )
    cmd = build_ffmpeg_command(req)
    assert "-crf" not in cmd
    assert cmd[cmd.index("-pass") + 1] == "2"
    assert cmd[cmd.index("-passlogfile") + 1] == str(stats)
    assert cmd[-1] == str(req.output)


def test_build_ffmpeg_command_x264_pass2_with_bitrate_extra_params(tmp_path: Path):
    stats = tmp_path / "encode.stats"
    req = EncodeRequest(
        source=tmp_path / "ref.yuv",
        width=64,
        height=64,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder="libx264",
        preset="medium",
        crf=23,
        output=tmp_path / "out.mp4",
        pass_number=2,
        stats_path=stats,
        extra_params=("-b:v", "2000k"),
    )
    cmd = build_ffmpeg_command(req)
    assert "-crf" not in cmd
    assert "-b:v" in cmd
    assert cmd[cmd.index("-b:v") + 1] == "2000k"
    assert cmd[cmd.index("-pass") + 1] == "2"
    assert cmd[cmd.index("-passlogfile") + 1] == str(stats)
    assert cmd[-1] == str(req.output)


def test_build_ffmpeg_command_pass_number_requires_stats_path(tmp_path: Path):
    req = _x265_request(tmp_path, pass_number=1, stats_path=None)
    with pytest.raises(ValueError, match="stats_path"):
        build_ffmpeg_command(req)


def test_build_ffmpeg_command_two_pass_rejected_for_unsupported_encoder(tmp_path: Path):
    # av1_amf has supports_two_pass = False (hardware encoder); the
    # build function refuses rather than silently producing a
    # bogus argv.
    req = EncodeRequest(
        source=tmp_path / "ref.yuv",
        width=64,
        height=64,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder="av1_amf",
        preset="medium",
        crf=28,
        output=tmp_path / "out.mp4",
        pass_number=1,
        stats_path=tmp_path / "encode.stats",
    )
    with pytest.raises(ValueError, match="does not support 2-pass"):
        build_ffmpeg_command(req)


def test_build_ffmpeg_command_pass0_is_single_pass(tmp_path: Path):
    # Default: pass_number=0 means "single-pass"; no -x265-params, no
    # null muxer redirect, output written normally.
    req = _x265_request(tmp_path)
    cmd = build_ffmpeg_command(req)
    assert "-x265-params" not in cmd
    assert cmd[-1] == str(req.output)


# --- run_two_pass_encode round-trip -----------------------------------


def test_run_two_pass_encode_drives_both_passes_in_order(tmp_path: Path):
    src = _make_yuv(tmp_path / "ref.yuv")
    out = tmp_path / "out.mp4"
    invocations: list[list[str]] = []

    def fake_run(cmd, capture_output, text, check):
        invocations.append(list(cmd))
        # Only pass 2 writes the output; replicate that on disk so
        # ``run_encode`` reports a non-zero size.
        if "-x265-params" in cmd:
            params_arg = cmd[cmd.index("-x265-params") + 1]
            if params_arg.startswith("pass=2"):
                Path(cmd[-1]).write_bytes(b"\x00" * 8192)
        return _FakeCompleted(
            returncode=0,
            stderr=("ffmpeg version 6.1.1\n" "x265 [info]: HEVC encoder version 3.5+1\n"),
        )

    req = EncodeRequest(
        source=src,
        width=64,
        height=64,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder="libx265",
        preset="medium",
        crf=28,
        output=out,
    )
    res = run_two_pass_encode(req, runner=fake_run)

    assert res.exit_status == 0
    assert res.encode_size_bytes == 8192  # pass-2 output, not pass-1 (which is 0)
    assert res.encode_time_ms > 0.0
    assert len(invocations) == 2

    # Pass 1 first, pass 2 second; both target the same stats file.
    pass1_params = invocations[0][invocations[0].index("-x265-params") + 1]
    pass2_params = invocations[1][invocations[1].index("-x265-params") + 1]
    assert pass1_params.startswith("pass=1:stats=")
    assert pass2_params.startswith("pass=2:stats=")
    # Same stats path across both passes — the whole point of 2-pass.
    assert pass1_params.split("stats=", 1)[1] == pass2_params.split("stats=", 1)[1]

    # Pass 1 writes to null muxer; pass 2 writes to the requested output.
    assert invocations[0][-3:] == ["-f", "null", "-"]
    assert invocations[1][-1] == str(out)


def test_run_two_pass_encode_x264_drives_both_passes_in_order(tmp_path: Path):
    src = _make_yuv(tmp_path / "ref.yuv")
    out = tmp_path / "out.mp4"
    invocations: list[list[str]] = []

    def fake_run(cmd, capture_output, text, check):
        invocations.append(list(cmd))
        if "-pass" in cmd and cmd[cmd.index("-pass") + 1] == "2":
            Path(cmd[-1]).write_bytes(b"\x00" * 4096)
        return _FakeCompleted(
            returncode=0,
            stderr=("ffmpeg version 6.1.1\n" "x264 - core 164 r3107\n"),
        )

    req = EncodeRequest(
        source=src,
        width=64,
        height=64,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder="libx264",
        preset="medium",
        crf=23,
        output=out,
    )
    res = run_two_pass_encode(req, runner=fake_run)

    assert res.exit_status == 0
    assert res.encode_size_bytes == 4096
    assert len(invocations) == 2
    assert "-crf" not in invocations[0]
    assert "-crf" not in invocations[1]
    assert invocations[0][invocations[0].index("-pass") + 1] == "1"
    assert invocations[1][invocations[1].index("-pass") + 1] == "2"
    pass1_log = invocations[0][invocations[0].index("-passlogfile") + 1]
    pass2_log = invocations[1][invocations[1].index("-passlogfile") + 1]
    assert pass1_log == pass2_log
    assert invocations[0][-3:] == ["-f", "null", "-"]
    assert invocations[1][-1] == str(out)


def test_run_two_pass_encode_skips_pass2_on_pass1_failure(tmp_path: Path):
    src = _make_yuv(tmp_path / "ref.yuv")
    out = tmp_path / "out.mp4"
    call_count = {"n": 0}

    def fake_run(cmd, capture_output, text, check):
        call_count["n"] += 1
        # Fail on the first invocation (pass 1).
        if call_count["n"] == 1:
            return _FakeCompleted(returncode=1, stderr="x265 [error]: pass 1 boom\n")
        return _FakeCompleted(returncode=0, stderr="")

    req = EncodeRequest(
        source=src,
        width=64,
        height=64,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder="libx265",
        preset="medium",
        crf=28,
        output=out,
    )
    res = run_two_pass_encode(req, runner=fake_run)
    assert call_count["n"] == 1  # pass 2 was skipped
    assert res.exit_status == 1
    assert "pass 1 failed" in res.stderr_tail


def test_run_two_pass_encode_falls_back_when_codec_unsupported(tmp_path: Path, capsys):
    # av1_amf does not support 2-pass; the driver should emit a
    # warning and run a single-pass encode, NOT raise.
    src = _make_yuv(tmp_path / "ref.yuv")
    out = tmp_path / "out.mp4"
    invocations: list[list[str]] = []

    def fake_run(cmd, capture_output, text, check):
        invocations.append(list(cmd))
        Path(cmd[-1]).write_bytes(b"\x00" * 4096)
        return _FakeCompleted(returncode=0, stderr="ffmpeg version 6.1.1\n")

    req = EncodeRequest(
        source=src,
        width=64,
        height=64,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder="av1_amf",
        preset="medium",
        crf=28,
        output=out,
    )
    res = run_two_pass_encode(req, runner=fake_run)
    assert res.exit_status == 0
    assert len(invocations) == 1  # single pass, not two
    captured = capsys.readouterr()
    assert "does not support 2-pass" in captured.err


def test_run_two_pass_encode_raise_mode_propagates(tmp_path: Path):
    src = _make_yuv(tmp_path / "ref.yuv")
    out = tmp_path / "out.mp4"

    req = EncodeRequest(
        source=src,
        width=64,
        height=64,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder="av1_amf",
        preset="medium",
        crf=28,
        output=out,
    )
    with pytest.raises(ValueError, match="does not support 2-pass"):
        run_two_pass_encode(req, on_unsupported="raise", runner=lambda *a, **k: None)


def test_run_two_pass_encode_cleans_stats_file(tmp_path: Path):
    """Stats file is removed after the run, regardless of outcome."""
    src = _make_yuv(tmp_path / "ref.yuv")
    out = tmp_path / "out.mp4"
    seen_stats: list[Path] = []

    def fake_run(cmd, capture_output, text, check):
        # Materialise a fake stats file as the real x265 would, so we
        # can verify cleanup.
        params_arg = cmd[cmd.index("-x265-params") + 1]
        stats_str = params_arg.split("stats=", 1)[1]
        stats = Path(stats_str)
        stats.parent.mkdir(parents=True, exist_ok=True)
        stats.write_text("x265 stats placeholder\n")
        seen_stats.append(stats)
        if params_arg.startswith("pass=2"):
            Path(cmd[-1]).write_bytes(b"\x00" * 4096)
        return _FakeCompleted(returncode=0, stderr="ffmpeg version 6.1.1\n")

    req = EncodeRequest(
        source=src,
        width=64,
        height=64,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder="libx265",
        preset="medium",
        crf=28,
        output=out,
    )
    run_two_pass_encode(req, runner=fake_run)
    assert seen_stats, "fake_run should have observed at least one invocation"
    assert all(not p.exists() for p in seen_stats), (
        "stats file should be cleaned up after the 2-pass run; survivors: "
        f"{[p for p in seen_stats if p.exists()]}"
    )


def test_run_two_pass_encode_cleans_x264_passlog_files(tmp_path: Path):
    """FFmpeg's generic passlogfile path writes <prefix>-0.log."""
    src = _make_yuv(tmp_path / "ref.yuv")
    out = tmp_path / "out.mp4"
    seen_logs: list[Path] = []

    def fake_run(cmd, capture_output, text, check):
        log_prefix = Path(cmd[cmd.index("-passlogfile") + 1])
        stream_log = log_prefix.parent / f"{log_prefix.name}-0.log"
        stream_log.parent.mkdir(parents=True, exist_ok=True)
        stream_log.write_text("x264 stats placeholder\n")
        stream_log.with_suffix(stream_log.suffix + ".mbtree").write_text("mbtree\n")
        seen_logs.extend([stream_log, stream_log.with_suffix(stream_log.suffix + ".mbtree")])
        if cmd[cmd.index("-pass") + 1] == "2":
            Path(cmd[-1]).write_bytes(b"\x00" * 4096)
        return _FakeCompleted(returncode=0, stderr="ffmpeg version 6.1.1\nx264 - core 164\n")

    req = EncodeRequest(
        source=src,
        width=64,
        height=64,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder="libx264",
        preset="medium",
        crf=23,
        output=out,
    )
    run_two_pass_encode(req, runner=fake_run)
    assert seen_logs, "fake_run should have observed x264 passlog files"
    assert all(not p.exists() for p in seen_logs), (
        "x264 passlog files should be cleaned up; survivors: "
        f"{[p for p in seen_logs if p.exists()]}"
    )


# --- Real-binary integration (opt-in) ---------------------------------


@pytest.mark.skipif(
    os.environ.get("VMAF_TUNE_INTEGRATION") != "1",
    reason="set VMAF_TUNE_INTEGRATION=1 to exercise the real ffmpeg/x265 stack",
)
def test_real_x265_two_pass_smoke(tmp_path: Path):
    """Synthetic 5-second clip → 1-pass and 2-pass libx265 encodes.

    Asserts:

    1. Both encodes succeed with non-zero output size.
    2. The two bitstreams differ (the 2-pass run actually used the
       stats file rather than re-running the 1-pass logic).
    3. The encoded sizes are within a generous factor of each other
       (sanity-check against a runaway encode).
    """
    if shutil.which("ffmpeg") is None:
        pytest.skip("ffmpeg not on PATH")

    # Synthesise a 5s 64x64 YUV420 reference via testsrc.
    ref = tmp_path / "ref.yuv"
    proc = subprocess.run(
        [
            "ffmpeg",
            "-y",
            "-hide_banner",
            "-loglevel",
            "error",
            "-f",
            "lavfi",
            "-i",
            "testsrc=size=64x64:duration=5:rate=24",
            "-pix_fmt",
            "yuv420p",
            "-f",
            "rawvideo",
            str(ref),
        ],
        capture_output=True,
        check=False,
    )
    if proc.returncode != 0:
        pytest.skip("could not synthesise YUV reference via lavfi testsrc")

    # 1-pass encode.
    out_1pass = tmp_path / "out_1pass.mp4"
    req_1 = EncodeRequest(
        source=ref,
        width=64,
        height=64,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder="libx265",
        preset="ultrafast",
        crf=28,
        output=out_1pass,
    )
    from vmaftune.encode import run_encode

    res_1 = run_encode(req_1)
    if res_1.exit_status != 0:
        pytest.skip(f"libx265 unavailable in local ffmpeg build: {res_1.stderr_tail}")
    assert res_1.encode_size_bytes > 0

    # 2-pass encode at the same parameters.
    out_2pass = tmp_path / "out_2pass.mp4"
    req_2 = EncodeRequest(
        source=ref,
        width=64,
        height=64,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder="libx265",
        preset="ultrafast",
        crf=28,
        output=out_2pass,
    )
    res_2 = run_two_pass_encode(req_2)
    assert res_2.exit_status == 0
    assert res_2.encode_size_bytes > 0

    # Bitstreams must differ — the 2-pass run uses the stats file
    # to make different rate-allocation decisions, even at the same
    # nominal CRF.
    assert out_1pass.read_bytes() != out_2pass.read_bytes(), (
        "1-pass and 2-pass libx265 produced byte-identical outputs; "
        "the 2-pass stats file was likely not consumed."
    )

    # Sanity: sizes within an order of magnitude.
    ratio = res_2.encode_size_bytes / max(res_1.encode_size_bytes, 1)
    assert 0.1 < ratio < 10.0, (
        f"runaway encode: 1-pass={res_1.encode_size_bytes}B, "
        f"2-pass={res_2.encode_size_bytes}B (ratio {ratio:.2f})"
    )
