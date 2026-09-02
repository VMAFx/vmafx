# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""End-to-end corpus row test for encoder-stats columns (ADR-0332).

Mocks ffmpeg + vmaf + the x264 stats file, then drives ``iter_rows``
and asserts the JSONL row carries non-zero ``enc_internal_*`` values.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune import CORPUS_ROW_KEYS, SCHEMA_VERSION
from vmaftune.corpus import CorpusJob, CorpusOptions, iter_rows

_X264_STATS_FIXTURE = (
    "#options: 64x64 fps=24/1 timebase=1/24 bitdepth=8\n"
    "in:0 out:0 type:I dur:1 cpbdur:1 q:23.00 aq:20.00 tex:8000 mv:0 misc:300 "
    "imb:16 pmb:0 smb:0 d:- ref:;\n"
    "in:1 out:1 type:P dur:1 cpbdur:1 q:23.00 aq:21.00 tex:1500 mv:60 misc:90 "
    "imb:1 pmb:10 smb:5 d:- ref:0 ;\n"
)


class _FakeCompleted:
    def __init__(self, returncode: int = 0, stderr: str = ""):
        self.returncode = returncode
        self.stdout = ""
        self.stderr = stderr


def test_x264_corpus_row_includes_encoder_stats(tmp_path: Path):
    src = tmp_path / "ref.yuv"
    src.write_bytes(b"\x80" * 4096)

    def fake_encode_run(cmd, capture_output, text, check):
        # Pass-1: drop the canned stats file at <prefix>-0.log.
        if "-pass" in cmd and cmd[cmd.index("-pass") + 1] == "1":
            prefix = Path(cmd[cmd.index("-passlogfile") + 1])
            log = prefix.parent / f"{prefix.name}-0.log"
            log.parent.mkdir(parents=True, exist_ok=True)
            log.write_text(_X264_STATS_FIXTURE)
            return _FakeCompleted(returncode=0)
        # Main encode.
        out_path = Path(cmd[-1])
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_bytes(b"\x00" * 2048)
        return _FakeCompleted(
            returncode=0,
            stderr="ffmpeg version 6.1.1\nx264 - core 164 r3107\n",
        )

    def fake_score_run(cmd, capture_output, text, check):
        out_idx = cmd.index("--output") + 1
        out_path = Path(cmd[out_idx])
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(json.dumps({"pooled_metrics": {"vmaf": {"mean": 88.5}}}))
        return _FakeCompleted(returncode=0, stderr="VMAF version: 3.0.0-lusoris\n")

    job = CorpusJob(
        source=src,
        width=64,
        height=64,
        pix_fmt="yuv420p",
        framerate=24.0,
        duration_s=2.0,
        cells=(("medium", 23),),
    )
    opts = CorpusOptions(
        output=tmp_path / "corpus.jsonl",
        encode_dir=tmp_path / "encodes",
        keep_encodes=False,
        src_sha256=False,
    )

    rows = list(
        iter_rows(
            job,
            opts,
            encode_runner=fake_encode_run,
            score_runner=fake_score_run,
        )
    )
    assert len(rows) == 1
    row = rows[0]

    # Schema-shape contract.
    assert set(CORPUS_ROW_KEYS) == set(row.keys())
    assert row["schema_version"] == SCHEMA_VERSION

    # Encoder-stats columns populated from the canned fixture.
    # I-frame tex 8000, P-frame tex 1500 — itex_mean = 8000, ptex_mean = 1500.
    assert row["enc_internal_itex_mean"] == pytest.approx(8000.0)
    assert row["enc_internal_ptex_mean"] == pytest.approx(1500.0)
    assert row["enc_internal_qp_mean"] == pytest.approx(23.0)
    # bits: I = 8000+0+300 = 8300; P = 1500+60+90 = 1650; mean = 4975
    assert row["enc_internal_bits_mean"] == pytest.approx((8300 + 1650) / 2.0)
    # mb totals: I 16 intra, P 1 intra + 10 pred + 5 skip = 16 → total 32
    assert row["enc_internal_intra_ratio"] == pytest.approx(17.0 / 32.0)
    assert row["enc_internal_skip_ratio"] == pytest.approx(5.0 / 32.0)


def test_hardware_encoder_corpus_row_emits_zero_encoder_stats(tmp_path: Path):
    """NVENC etc. set ``supports_encoder_stats=False`` → zero columns.

    Confirms the schema is uniform across codecs even though the
    hardware encoder never runs the pass-1 stats path.
    """
    src = tmp_path / "ref.yuv"
    src.write_bytes(b"\x80" * 4096)

    pass1_seen = {"flag": False}

    def fake_encode_run(cmd, capture_output, text, check):
        if "-pass" in cmd and cmd[cmd.index("-pass") + 1] == "1":
            pass1_seen["flag"] = True
        out_path = Path(cmd[-1])
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_bytes(b"\x00" * 2048)
        return _FakeCompleted(
            returncode=0,
            stderr="ffmpeg version 6.1.1\nNVENC version 12.0\n",
        )

    def fake_score_run(cmd, capture_output, text, check):
        out_idx = cmd.index("--output") + 1
        out_path = Path(cmd[out_idx])
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(json.dumps({"pooled_metrics": {"vmaf": {"mean": 90.0}}}))
        return _FakeCompleted(returncode=0, stderr="VMAF version: 3.0.0-lusoris\n")

    job = CorpusJob(
        source=src,
        width=64,
        height=64,
        pix_fmt="yuv420p",
        framerate=24.0,
        duration_s=2.0,
        cells=(("medium", 23),),
    )
    opts = CorpusOptions(
        encoder="h264_nvenc",
        output=tmp_path / "corpus.jsonl",
        encode_dir=tmp_path / "encodes",
        keep_encodes=False,
        src_sha256=False,
    )

    rows = list(
        iter_rows(
            job,
            opts,
            encode_runner=fake_encode_run,
            score_runner=fake_score_run,
        )
    )
    assert len(rows) == 1
    row = rows[0]

    # NVENC must not have triggered a pass-1 invocation.
    assert pass1_seen["flag"] is False

    # All ten encoder-stats columns present and zero.
    for col in (
        "enc_internal_qp_mean",
        "enc_internal_qp_std",
        "enc_internal_bits_mean",
        "enc_internal_bits_std",
        "enc_internal_mv_mean",
        "enc_internal_mv_std",
        "enc_internal_itex_mean",
        "enc_internal_ptex_mean",
        "enc_internal_intra_ratio",
        "enc_internal_skip_ratio",
    ):
        assert row[col] == 0.0


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
