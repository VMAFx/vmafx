# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Smoke tests for the FR-from-NR adapter (ADR-0346).

All ffprobe / ffmpeg / vmaf invocations are mocked via the
runner-injection seams; the suite has zero filesystem dependency on
real binaries.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

# Make src/ importable without an editable install.
_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune import CORPUS_ROW_KEYS  # noqa: E402
from vmaftune.corpus import CorpusOptions  # noqa: E402
from vmaftune.fr_from_nr_adapter import (  # noqa: E402
    DEFAULT_CRF_SWEEP,
    DEFAULT_PRESET,
    NrInputRow,
    NrSourceGeometry,
    NrToFrAdapter,
    build_decode_command,
    probe_geometry,
)


class _FakeCompleted:
    """Minimal stand-in for ``subprocess.CompletedProcess``."""

    def __init__(self, returncode: int, stdout: str = "", stderr: str = ""):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


def _ffprobe_payload(
    *,
    width: int = 1280,
    height: int = 720,
    pix_fmt: str = "yuv420p",
    rate: str = "30/1",
    duration: str = "12.0",
) -> str:
    return json.dumps(
        {
            "streams": [
                {
                    "width": width,
                    "height": height,
                    "pix_fmt": pix_fmt,
                    "r_frame_rate": rate,
                }
            ],
            "format": {"duration": duration},
        }
    )


def _make_fake_runners(tmp_path: Path):
    """Return (probe, decode, encode, score) runners that emulate a happy path."""
    captured: dict[str, list[list[str]]] = {
        "probe": [],
        "decode": [],
        "encode": [],
        "score": [],
    }

    def probe_runner(cmd, capture_output, text, check):  # noqa: ARG001
        captured["probe"].append(list(cmd))
        return _FakeCompleted(returncode=0, stdout=_ffprobe_payload())

    def decode_runner(cmd, capture_output, text, check):  # noqa: ARG001
        captured["decode"].append(list(cmd))
        # The adapter expects the decoded YUV to exist after this call
        # so the FR encoder can read it. Write a stub byte stream.
        out_yuv = Path(cmd[-1])
        out_yuv.parent.mkdir(parents=True, exist_ok=True)
        out_yuv.write_bytes(b"\x80" * 4096)
        return _FakeCompleted(returncode=0, stderr="ffmpeg version 6.1.1\n")

    def encode_runner(cmd, capture_output, text, check):  # noqa: ARG001
        captured["encode"].append(list(cmd))
        Path(cmd[-1]).write_bytes(b"\x00" * 8192)
        return _FakeCompleted(
            returncode=0,
            stderr="ffmpeg version 6.1.1\nx264 - core 164 r3107\n",
        )

    def score_runner(cmd, capture_output, text, check):  # noqa: ARG001
        captured["score"].append(list(cmd))
        out_idx = cmd.index("--output") + 1
        out_path = Path(cmd[out_idx])
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(json.dumps({"pooled_metrics": {"vmaf": {"mean": 88.5}}}))
        return _FakeCompleted(returncode=0, stderr="VMAF version: 3.0.0-lusoris\n")

    return captured, probe_runner, decode_runner, encode_runner, score_runner


def test_default_crf_sweep_is_five_points():
    assert DEFAULT_CRF_SWEEP == (18, 23, 28, 33, 38)
    assert DEFAULT_PRESET == "medium"


def test_nr_input_row_from_dict_round_trips_extras():
    row = NrInputRow.from_dict(
        {"src": "/tmp/clip.mp4", "mos": 4.1, "video_id": "abc", "category": "ugc"}
    )
    assert row.src == Path("/tmp/clip.mp4")
    assert row.mos == 4.1
    assert row.extra == {"video_id": "abc", "category": "ugc"}


def test_nr_input_row_from_dict_rejects_missing_src():
    with pytest.raises(KeyError):
        NrInputRow.from_dict({"mos": 3.5})


def test_probe_geometry_parses_ffprobe_payload(tmp_path: Path):
    src = tmp_path / "clip.mp4"
    src.write_bytes(b"\x00")

    def runner(cmd, capture_output, text, check):  # noqa: ARG001
        assert cmd[0] == "ffprobe"
        assert str(src) in cmd
        return _FakeCompleted(
            returncode=0,
            stdout=_ffprobe_payload(
                width=1920, height=1080, pix_fmt="yuv420p10le", rate="24000/1001"
            ),
        )

    geom = probe_geometry(src, runner=runner)
    assert geom == NrSourceGeometry(
        width=1920,
        height=1080,
        pix_fmt="yuv420p10le",
        framerate=pytest.approx(24000 / 1001, rel=1e-9),
        duration_s=12.0,
    )


def test_probe_geometry_raises_on_invalid_geometry(tmp_path: Path):
    def runner(cmd, capture_output, text, check):  # noqa: ARG001
        return _FakeCompleted(
            returncode=0,
            stdout=json.dumps({"streams": [{"width": 0, "height": 0}], "format": {}}),
        )

    with pytest.raises(RuntimeError, match="invalid geometry"):
        probe_geometry(tmp_path / "x.mp4", runner=runner)


def test_probe_geometry_raises_on_nonzero_exit(tmp_path: Path):
    def runner(cmd, capture_output, text, check):  # noqa: ARG001
        return _FakeCompleted(returncode=1, stderr="boom")

    with pytest.raises(RuntimeError, match="ffprobe failed"):
        probe_geometry(tmp_path / "x.mp4", runner=runner)


def test_build_decode_command_uses_probed_pix_fmt():
    geom = NrSourceGeometry(
        width=1280, height=720, pix_fmt="yuv422p", framerate=24.0, duration_s=10.0
    )
    cmd = build_decode_command(Path("in.mp4"), Path("out.yuv"), geom, ffmpeg_bin="ffmpeg")
    assert cmd[0] == "ffmpeg"
    assert "-i" in cmd
    assert cmd[cmd.index("-i") + 1] == "in.mp4"
    assert "-f" in cmd
    assert cmd[cmd.index("-f") + 1] == "rawvideo"
    assert "-pix_fmt" in cmd
    assert cmd[cmd.index("-pix_fmt") + 1] == "yuv422p"
    assert cmd[-1] == "out.yuv"


def test_adapter_validates_construction_args():
    with pytest.raises(ValueError, match="non-empty"):
        NrToFrAdapter(crf_sweep=())
    with pytest.raises(TypeError):
        NrToFrAdapter(crf_sweep=(23.0,))  # type: ignore[arg-type]
    with pytest.raises(ValueError):
        NrToFrAdapter(preset="")


def test_adapter_run_yields_one_row_per_crf(tmp_path: Path):
    captured, probe_r, decode_r, encode_r, score_r = _make_fake_runners(tmp_path)

    src = tmp_path / "ugc_clip.mp4"
    src.write_bytes(b"\x00")
    nr_row = NrInputRow(src=src, mos=4.2)

    sweep = (20, 30, 40)
    adapter = NrToFrAdapter(
        crf_sweep=sweep,
        preset="fast",
        scratch_dir=tmp_path / "scratch",
        keep_intermediate_yuv=False,
        options=CorpusOptions(
            output=tmp_path / "out.jsonl",
            encode_dir=tmp_path / "encodes",
            keep_encodes=False,
            src_sha256=False,
        ),
    )
    rows = list(
        adapter.run(
            nr_row,
            probe_runner=probe_r,
            decode_runner=decode_r,
            encode_runner=encode_r,
            score_runner=score_r,
        )
    )

    # Exactly one row per CRF in the sweep, in deterministic order.
    assert len(rows) == len(sweep)
    assert [r["crf"] for r in rows] == list(sweep)
    assert all(r["preset"] == "fast" for r in rows)
    assert all(r["encoder"] == "libx264" for r in rows)
    # Schema invariant: every row carries the canonical row-key set.
    for row in rows:
        for key in CORPUS_ROW_KEYS:
            assert key in row, f"row missing canonical key {key!r}"
    # Provenance annotations are present and correct.
    assert all(r["fr_from_nr"] is True for r in rows)
    assert all(r["nr_source"] == str(src) for r in rows)
    assert all(r["nr_mos"] == 4.2 for r in rows)
    # The pipeline used the mocked seams (real ffprobe / ffmpeg / vmaf
    # never invoked). The x264 adapter has `supports_encoder_stats=True`
    # so iter_rows calls run_encode_with_stats, which runs FFmpeg twice
    # per CRF (pass-1 stats-only to /dev/null, then the real CRF encode).
    # Score is called once per CRF against the real encode output.
    assert len(captured["probe"]) == 1
    assert len(captured["decode"]) == 1
    assert len(captured["encode"]) == 2 * len(sweep)
    assert len(captured["score"]) == len(sweep)


def test_adapter_cleans_intermediate_yuv_by_default(tmp_path: Path):
    _, probe_r, decode_r, encode_r, score_r = _make_fake_runners(tmp_path)

    src = tmp_path / "ugc_clip.mp4"
    src.write_bytes(b"\x00")
    adapter = NrToFrAdapter(
        crf_sweep=(23,),
        scratch_dir=tmp_path / "scratch",
        keep_intermediate_yuv=False,
        options=CorpusOptions(
            output=tmp_path / "out.jsonl",
            encode_dir=tmp_path / "encodes",
            src_sha256=False,
        ),
    )
    list(
        adapter.run(
            NrInputRow(src=src),
            probe_runner=probe_r,
            decode_runner=decode_r,
            encode_runner=encode_r,
            score_runner=score_r,
        )
    )
    intermediate = tmp_path / "scratch" / "ugc_clip.yuv"
    assert not intermediate.exists(), "intermediate YUV should be cleaned by default"


def test_adapter_keeps_intermediate_yuv_when_requested(tmp_path: Path):
    _, probe_r, decode_r, encode_r, score_r = _make_fake_runners(tmp_path)

    src = tmp_path / "ugc_clip.mp4"
    src.write_bytes(b"\x00")
    adapter = NrToFrAdapter(
        crf_sweep=(23,),
        scratch_dir=tmp_path / "scratch",
        keep_intermediate_yuv=True,
        options=CorpusOptions(
            output=tmp_path / "out.jsonl",
            encode_dir=tmp_path / "encodes",
            src_sha256=False,
        ),
    )
    list(
        adapter.run(
            NrInputRow(src=src),
            probe_runner=probe_r,
            decode_runner=decode_r,
            encode_runner=encode_r,
            score_runner=score_r,
        )
    )
    intermediate = tmp_path / "scratch" / "ugc_clip.yuv"
    assert intermediate.exists(), "intermediate YUV should persist when requested"


def test_adapter_run_many_yields_in_deterministic_order(tmp_path: Path):
    _, probe_r, decode_r, encode_r, score_r = _make_fake_runners(tmp_path)

    srcs = []
    for stem in ("a", "b", "c"):
        p = tmp_path / f"{stem}.mp4"
        p.write_bytes(b"\x00")
        srcs.append(NrInputRow(src=p, mos=3.0))

    adapter = NrToFrAdapter(
        crf_sweep=(23, 28),
        scratch_dir=tmp_path / "scratch",
        options=CorpusOptions(
            output=tmp_path / "out.jsonl",
            encode_dir=tmp_path / "encodes",
            src_sha256=False,
        ),
    )
    rows = list(
        adapter.run_many(
            srcs,
            probe_runner=probe_r,
            decode_runner=decode_r,
            encode_runner=encode_r,
            score_runner=score_r,
        )
    )
    # 3 sources × 2 CRFs = 6 rows, grouped by source in input order.
    assert len(rows) == 6
    seen_sources = [r["nr_source"] for r in rows]
    assert seen_sources == [
        str(tmp_path / "a.mp4"),
        str(tmp_path / "a.mp4"),
        str(tmp_path / "b.mp4"),
        str(tmp_path / "b.mp4"),
        str(tmp_path / "c.mp4"),
        str(tmp_path / "c.mp4"),
    ]


def test_adapter_propagates_decode_failure(tmp_path: Path):
    captured, probe_r, _, encode_r, score_r = _make_fake_runners(tmp_path)

    def failing_decode(cmd, capture_output, text, check):  # noqa: ARG001
        captured["decode"].append(list(cmd))
        return _FakeCompleted(returncode=1, stderr="decode boom")

    src = tmp_path / "broken.mp4"
    src.write_bytes(b"\x00")
    adapter = NrToFrAdapter(
        crf_sweep=(23,),
        scratch_dir=tmp_path / "scratch",
        options=CorpusOptions(
            output=tmp_path / "out.jsonl",
            encode_dir=tmp_path / "encodes",
            src_sha256=False,
        ),
    )
    with pytest.raises(RuntimeError, match="ffmpeg decode failed"):
        list(
            adapter.run(
                NrInputRow(src=src),
                probe_runner=probe_r,
                decode_runner=failing_decode,
                encode_runner=encode_r,
                score_runner=score_r,
            )
        )
    # The encoder / scorer must NOT have been invoked.
    assert captured["encode"] == []
    assert captured["score"] == []
