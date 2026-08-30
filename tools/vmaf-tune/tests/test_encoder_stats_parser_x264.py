# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""x264 stats-file parser unit tests (ADR-0332).

The fixture lines in this file were captured verbatim from x264
0.165.r3214 driven through ``ffmpeg -pass 1 -passlogfile <prefix>``
on a 2-second ``testsrc=160x120@10`` clip. They are the canonical
shape every libx264 build emits — keep the fixtures in sync with the
parser docstring in ``vmaftune/encoder_stats.py``.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.encoder_stats import (
    ENCODER_STATS_COLUMNS,
    PerFrameStats,
    aggregate_stats,
    parse_stats_file,
    parse_stats_line,
)

# Real x264 stats lines captured via:
#   ffmpeg -f lavfi -i testsrc=duration=2:size=160x120:rate=10 \
#          -c:v libx264 -pass 1 -passlogfile /tmp/x /tmp/x.mp4
_OPTIONS_HEADER = (
    "#options: 160x120 fps=10/1 timebase=1/10 bitdepth=8 cabac=1 ref=1 "
    "deblock=1:0:0 analyse=0x1:0 me=dia subme=2 psy=1 psy_rd=1.00:0.00 "
    "rc=crf mbtree=1 crf=23.0 qcomp=0.60 qpmin=0 qpmax=69 qpstep=4"
)
_I_FRAME = (
    "in:0 out:0 type:I dur:2 cpbdur:2 q:25.23 aq:20.39 "
    "tex:14051 mv:1126 misc:5871 imb:80 pmb:0 smb:0 d:- ref:;"
)
_P_FRAME = (
    "in:1 out:1 type:P dur:2 cpbdur:2 q:25.23 aq:23.76 "
    "tex:2595 mv:151 misc:182 imb:0 pmb:33 smb:47 d:- ref:0 ;"
)
_b_FRAME = (
    "in:3 out:4 type:b dur:2 cpbdur:2 q:25.23 aq:14.02 "
    "tex:58 mv:3 misc:115 imb:0 pmb:1 smb:79 d:- ref:0 ;"
)
_X265_I_FRAME = (
    "in:0 out:0 type:I q:27.83 q-aq:28.41 q-noVbv:27.83 q-Rceq:0.53 "
    "tex:11836 mv:1752 misc:129 icu:80.00 pcu:0.00 scu:0.00 sc:0 ;"
)
_X265_P_FRAME = (
    "in:1 out:1 type:P q:27.83 q-aq:28.71 q-noVbv:27.83 q-Rceq:0.53 "
    "tex:1317 mv:200 misc:191 icu:0.53 pcu:12.00 scu:67.47 sc:0 ;"
)


def test_parse_options_header_returns_none():
    assert parse_stats_line(_OPTIONS_HEADER) is None


def test_parse_blank_line_returns_none():
    assert parse_stats_line("") is None
    assert parse_stats_line("   \n") is None


def test_parse_i_frame_populates_all_ten_fields():
    rec = parse_stats_line(_I_FRAME)
    assert rec is not None
    # All ten task-spec fields populated.
    assert rec.in_idx == 0
    assert rec.out_idx == 0
    assert rec.frame_type == "I"
    assert rec.qp == pytest.approx(25.23)
    assert rec.aq == pytest.approx(20.39)
    assert rec.tex == 14051
    assert rec.mv == 1126
    assert rec.misc == 5871
    assert rec.imb == 80
    assert rec.pmb == 0
    assert rec.smb == 0
    assert rec.is_intra is True
    assert rec.bits == 14051 + 1126 + 5871
    assert rec.mb_total == 80


def test_parse_p_frame_classified_as_inter():
    rec = parse_stats_line(_P_FRAME)
    assert rec is not None
    assert rec.frame_type == "P"
    assert rec.is_intra is False
    assert rec.tex == 2595
    assert rec.mv == 151
    assert rec.imb == 0
    assert rec.pmb == 33
    assert rec.smb == 47


def test_parse_b_frame_classified_as_inter():
    rec = parse_stats_line(_b_FRAME)
    assert rec is not None
    assert rec.frame_type == "b"
    assert rec.is_intra is False


def test_parse_x265_i_frame_aliases_aq_and_coding_unit_counts():
    rec = parse_stats_line(_X265_I_FRAME)
    assert rec is not None
    assert rec.frame_type == "I"
    assert rec.qp == pytest.approx(27.83)
    assert rec.aq == pytest.approx(28.41)
    assert rec.tex == 11836
    assert rec.mv == 1752
    assert rec.misc == 129
    assert rec.imb == pytest.approx(80.0)
    assert rec.pmb == pytest.approx(0.0)
    assert rec.smb == pytest.approx(0.0)
    assert rec.mb_total == pytest.approx(80.0)


def test_parse_x265_p_frame_keeps_fractional_ctu_ratios():
    rec = parse_stats_line(_X265_P_FRAME)
    assert rec is not None
    assert rec.frame_type == "P"
    assert rec.is_intra is False
    assert rec.aq == pytest.approx(28.71)
    assert rec.imb == pytest.approx(0.53)
    assert rec.pmb == pytest.approx(12.00)
    assert rec.smb == pytest.approx(67.47)
    assert rec.mb_total == pytest.approx(80.0)


def test_parse_stats_file_round_trips_three_frames(tmp_path: Path):
    p = tmp_path / "x264.stats"
    p.write_text(f"{_OPTIONS_HEADER}\n{_I_FRAME}\n{_P_FRAME}\n{_b_FRAME}\n")
    frames = parse_stats_file(p)
    assert len(frames) == 3
    assert frames[0].frame_type == "I"
    assert frames[1].frame_type == "P"
    assert frames[2].frame_type == "b"


def test_parse_missing_file_returns_empty():
    frames = parse_stats_file(Path("/nonexistent/path/x264.stats"))
    assert frames == []


def test_aggregate_stats_emits_all_ten_columns():
    frames = [
        parse_stats_line(_I_FRAME),
        parse_stats_line(_P_FRAME),
        parse_stats_line(_b_FRAME),
    ]
    agg = aggregate_stats(f for f in frames if f is not None)
    assert set(agg.keys()) == set(ENCODER_STATS_COLUMNS)
    # qp identical across frames in this fixture → std == 0
    assert agg["enc_internal_qp_mean"] == pytest.approx(25.23)
    assert agg["enc_internal_qp_std"] == pytest.approx(0.0)
    # bits = tex + mv + misc per frame
    expected_bits_mean = ((14051 + 1126 + 5871) + (2595 + 151 + 182) + (58 + 3 + 115)) / 3.0
    assert agg["enc_internal_bits_mean"] == pytest.approx(expected_bits_mean)
    # itex / ptex split: I-frame tex vs (P + b)/2.
    assert agg["enc_internal_itex_mean"] == pytest.approx(14051.0)
    assert agg["enc_internal_ptex_mean"] == pytest.approx((2595 + 58) / 2.0)
    # macroblock ratios. Total MB = 80 + (33+47) + (1+79) = 240.
    # intra = 80, skip = 0 + 47 + 79 = 126.
    assert agg["enc_internal_intra_ratio"] == pytest.approx(80.0 / 240.0)
    assert agg["enc_internal_skip_ratio"] == pytest.approx(126.0 / 240.0)


def test_aggregate_stats_handles_x265_fractional_coding_unit_counts():
    frames = [
        parse_stats_line(_X265_I_FRAME),
        parse_stats_line(_X265_P_FRAME),
    ]
    agg = aggregate_stats(f for f in frames if f is not None)
    assert agg["enc_internal_qp_mean"] == pytest.approx(27.83)
    assert agg["enc_internal_itex_mean"] == pytest.approx(11836.0)
    assert agg["enc_internal_ptex_mean"] == pytest.approx(1317.0)
    assert agg["enc_internal_intra_ratio"] == pytest.approx((80.0 + 0.53) / 160.0)
    assert agg["enc_internal_skip_ratio"] == pytest.approx(67.47 / 160.0)


def test_aggregate_stats_empty_input_emits_zeros():
    agg = aggregate_stats([])
    assert agg == {col: 0.0 for col in ENCODER_STATS_COLUMNS}


def test_per_frame_stats_dataclass_is_frozen():
    import dataclasses as _dc

    rec = PerFrameStats(
        in_idx=0,
        out_idx=0,
        frame_type="I",
        qp=23.0,
        aq=20.0,
        tex=100,
        mv=10,
        misc=5,
        imb=10,
        pmb=0,
        smb=0,
    )
    with pytest.raises(_dc.FrozenInstanceError):
        rec.qp = 99.0  # type: ignore[misc]
