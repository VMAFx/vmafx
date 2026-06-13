# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""x265 codec adapter — smoke tests (ADR-0288).

Mocks ``subprocess.run`` end-to-end so the suite passes regardless of
whether ffmpeg / x265 is installed on the runner. Real-binary
integration coverage lives behind the ``VMAF_TUNE_INTEGRATION=1``
environment gate (skipped here when unset).
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

# Make src/ importable without an editable install.
_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune import CORPUS_ROW_KEYS  # noqa: E402
from vmaftune.codec_adapters import X265Adapter, get_adapter, known_codecs  # noqa: E402
from vmaftune.corpus import CorpusJob, CorpusOptions, iter_rows  # noqa: E402
from vmaftune.encode import (  # noqa: E402
    EncodeRequest,
    build_ffmpeg_command,
    parse_versions,
    run_encode,
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


# --- Adapter contract -------------------------------------------------


def test_x265_registered_alongside_x264():
    assert "libx265" in known_codecs()
    assert "libx264" in known_codecs()
    a = get_adapter("libx265")
    assert isinstance(a, X265Adapter)
    assert a.encoder == "libx265"
    assert a.quality_knob == "crf"
    assert a.invert_quality is True


def test_x265_preset_set_includes_placebo():
    a = get_adapter("libx265")
    # x265 ships ten presets (one more than x264 — adds ``placebo``).
    assert len(a.presets) == 10
    assert "placebo" in a.presets
    assert "ultrafast" in a.presets
    assert "medium" in a.presets


def test_x265_validate_accepts_canonical_inputs():
    a = get_adapter("libx265")
    a.validate("medium", 23)
    a.validate("placebo", 28)
    a.validate("ultrafast", 40)


def test_x265_validate_rejects_unknown_preset():
    a = get_adapter("libx265")
    with pytest.raises(ValueError):
        a.validate("nope", 23)


def test_x265_validate_rejects_out_of_range_crf():
    a = get_adapter("libx265")
    with pytest.raises(ValueError):
        a.validate("medium", 100)
    with pytest.raises(ValueError):
        a.validate("medium", -1)


def test_x265_profile_for_8bit_yuv420():
    a = X265Adapter()
    assert a.profile_for("yuv420p") == "main"


def test_x265_profile_for_10bit_yuv420():
    a = X265Adapter()
    assert a.profile_for("yuv420p10le") == "main10"


def test_x265_profile_for_unknown_pix_fmt_falls_back_to_main():
    a = X265Adapter()
    assert a.profile_for("nv12") == "main"


# --- ffmpeg argv shape ------------------------------------------------


def test_build_ffmpeg_command_routes_libx265():
    req = EncodeRequest(
        source=Path("ref.yuv"),
        width=1920,
        height=1080,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder="libx265",
        preset="slow",
        crf=28,
        output=Path("out.mkv"),
    )
    cmd = build_ffmpeg_command(req, ffmpeg_bin="ffmpeg")
    assert cmd[0] == "ffmpeg"
    assert "-c:v" in cmd
    assert cmd[cmd.index("-c:v") + 1] == "libx265"
    assert cmd[cmd.index("-preset") + 1] == "slow"
    assert cmd[cmd.index("-crf") + 1] == "28"
    assert cmd[-1] == "out.mkv"


# --- Version banner parsing ------------------------------------------


def test_parse_versions_extracts_x265_banner():
    stderr = (
        "ffmpeg version 6.1.1 built with gcc\n"
        "x265 [info]: HEVC encoder version 3.5+1-f0c1022b6\n"
        "x265 [info]: build info [Linux][GCC 13.2.0][64 bit] 8bit+10bit+12bit\n"
    )
    ffm, enc = parse_versions(stderr, encoder="libx265")
    assert ffm == "6.1.1"
    assert enc == "libx265-3.5+1-f0c1022b6"


def test_parse_versions_x265_returns_unknown_on_miss():
    # An x264 banner shouldn't satisfy an x265 query — keeps the column
    # honest in mixed-codec corpora.
    stderr = "ffmpeg version 6.1.1\nx264 - core 164 r3107\n"
    ffm, enc = parse_versions(stderr, encoder="libx265")
    assert ffm == "6.1.1"
    assert enc == "unknown"


def test_parse_versions_default_encoder_remains_x264():
    # Backward-compatible default: callers that omit ``encoder`` keep
    # the Phase A x264 behaviour.
    stderr = "ffmpeg version 6.1.1\nx264 - core 164 r3107\n"
    ffm, enc = parse_versions(stderr)
    assert ffm == "6.1.1"
    assert enc == "libx264-164"


# --- run_encode subprocess seam --------------------------------------


def test_run_encode_x265_populates_encode_result(tmp_path: Path):
    src = _make_yuv(tmp_path / "ref.yuv")
    out = tmp_path / "out.mkv"

    def fake_run(cmd, capture_output, text, check):
        Path(cmd[-1]).write_bytes(b"\x00" * 8192)
        return _FakeCompleted(
            returncode=0,
            stderr=("ffmpeg version 6.1.1\n" "x265 [info]: HEVC encoder version 3.5+1-f0c1022b6\n"),
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
    res = run_encode(req, runner=fake_run)
    assert res.exit_status == 0
    assert res.encode_size_bytes == 8192
    assert res.ffmpeg_version == "6.1.1"
    assert res.encoder_version == "libx265-3.5+1-f0c1022b6"
    assert res.encode_time_ms > 0.0


# --- Corpus end-to-end ------------------------------------------------


def test_corpus_end_to_end_with_x265_mocks(tmp_path: Path):
    src = _make_yuv(tmp_path / "ref.yuv")

    def fake_encode_run(cmd, capture_output, text, check):
        out_path = Path(cmd[-1])
        out_path.write_bytes(b"\x00" * 4096)
        return _FakeCompleted(
            returncode=0,
            stderr=("ffmpeg version 6.1.1\n" "x265 [info]: HEVC encoder version 3.5+1-f0c1022b6\n"),
        )

    def fake_score_run(cmd, capture_output, text, check):
        out_idx = cmd.index("--output") + 1
        out_path = Path(cmd[out_idx])
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(json.dumps({"pooled_metrics": {"vmaf": {"mean": 95.0}}}))
        return _FakeCompleted(returncode=0, stderr="VMAF version: 3.0.0-lusoris\n")

    job = CorpusJob(
        source=src,
        width=64,
        height=64,
        pix_fmt="yuv420p",
        framerate=24.0,
        duration_s=2.0,
        cells=(("medium", 23), ("placebo", 28)),
    )
    opts = CorpusOptions(
        encoder="libx265",
        output=tmp_path / "corpus.jsonl",
        encode_dir=tmp_path / "encodes",
        keep_encodes=False,
        src_sha256=False,
    )
    rows = list(iter_rows(job, opts, encode_runner=fake_encode_run, score_runner=fake_score_run))

    assert len(rows) == 2
    for r in rows:
        assert set(CORPUS_ROW_KEYS) == set(r.keys())
        assert r["encoder"] == "libx265"
        assert r["encoder_version"] == "libx265-3.5+1-f0c1022b6"
        assert r["vmaf_score"] == pytest.approx(95.0)
        assert r["exit_status"] == 0
    presets_emitted = sorted(r["preset"] for r in rows)
    assert presets_emitted == ["medium", "placebo"]


# --- Error handling: missing binary ----------------------------------


def test_run_encode_handles_missing_x265_binary(tmp_path: Path):
    src = _make_yuv(tmp_path / "ref.yuv")
    out = tmp_path / "out.mkv"

    def raising_run(cmd, capture_output, text, check):
        # ffmpeg compiled without x265 fails with a non-zero exit and a
        # message on stderr — the harness must surface this in the
        # EncodeResult, not raise.
        return _FakeCompleted(
            returncode=1,
            stderr="Unknown encoder 'libx265'\n",
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
    res = run_encode(req, runner=raising_run)
    assert res.exit_status == 1
    assert res.encode_size_bytes == 0
    assert "Unknown encoder" in res.stderr_tail


def test_run_encode_handles_filenotfounderror_when_ffmpeg_absent(tmp_path: Path):
    src = _make_yuv(tmp_path / "ref.yuv")
    out = tmp_path / "out.mkv"

    def fnf_run(cmd, capture_output, text, check):
        raise FileNotFoundError("ffmpeg binary not on PATH")

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
    # Bare propagation is acceptable — callers handle FileNotFoundError
    # at the harness boundary; pin the contract so a future refactor
    # doesn't silently swallow it.
    with pytest.raises(FileNotFoundError):
        run_encode(req, runner=fnf_run)


# --- Real-binary integration (opt-in) ---------------------------------


@pytest.mark.skipif(
    os.environ.get("VMAF_TUNE_INTEGRATION") != "1",
    reason="set VMAF_TUNE_INTEGRATION=1 to exercise the real ffmpeg/x265 stack",
)
def test_real_x265_smoke_when_runner_has_ffmpeg(tmp_path: Path):
    if shutil.which("ffmpeg") is None:
        pytest.skip("ffmpeg not on PATH")
    # Generate a tiny YUV420 reference via ffmpeg's testsrc filter.
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
            "testsrc=size=64x64:duration=0.2:rate=24",
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
    out = tmp_path / "out.mkv"
    req = EncodeRequest(
        source=ref,
        width=64,
        height=64,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder="libx265",
        preset="ultrafast",
        crf=28,
        output=out,
    )
    res = run_encode(req)
    if res.exit_status != 0:
        pytest.skip(f"libx265 unavailable in local ffmpeg build: {res.stderr_tail}")
    assert res.encode_size_bytes > 0
