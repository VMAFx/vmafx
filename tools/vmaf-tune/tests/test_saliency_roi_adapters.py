# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Saliency-aware ROI tests for x265 / SVT-AV1 / libaom / libvvenc adapters.

All ONNX inference is mocked via ``session_factory``; no onnxruntime
install required. All file I/O uses ``tmp_path``; no encoder binaries
required.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

np = pytest.importorskip("numpy")

from vmaftune.codec_adapters.libaom import LibaomAdapter  # noqa: E402
from vmaftune.codec_adapters.svtav1 import SvtAv1Adapter  # noqa: E402
from vmaftune.codec_adapters.vvenc import VVenCAdapter  # noqa: E402
from vmaftune.codec_adapters.x265 import X265Adapter  # noqa: E402
from vmaftune.saliency import (  # noqa: E402
    QP_OFFSET_MAX,
    QP_OFFSET_MIN,
    SVTAV1_SB_SIDE,
    VVENC_CTU_SIDE,
    X264_MB_SIDE,
    augment_extra_params_with_libaom_qpfile,
    augment_extra_params_with_svtav1_qpmap,
    augment_extra_params_with_vvenc_roi,
    augment_extra_params_with_x265_zones,
    reduce_qp_map_to_blocks,
    saliency_to_qp_map,
    write_svtav1_qpoffset_map,
    write_vvenc_roi_csv,
    write_x265_zones_arg,
)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _make_block_offsets(bh: int, bw: int, fill: int = -4) -> "np.ndarray":
    """Return a constant ``int32 [bh, bw]`` block-offset array."""
    return np.full((bh, bw), fill, dtype=np.int32)


def _make_saliency_offsets(width: int, height: int, block: int, fill: int = -4) -> "np.ndarray":
    """Build block offsets from a constant saliency mask."""
    mask = np.full((height, width), 0.9, dtype=np.float32)
    qp_map = saliency_to_qp_map(mask, baseline_qp=28, foreground_offset=fill)
    return reduce_qp_map_to_blocks(qp_map, block=block)


# ===========================================================================
# write_x265_zones_arg
# ===========================================================================


def test_write_x265_zones_arg_single_frame():
    blocks = _make_block_offsets(3, 4, fill=-4)
    result = write_x265_zones_arg(blocks, duration_frames=1)
    # Should be "0,0,q=<mean>"
    assert result.startswith("0,0,q=")


def test_write_x265_zones_arg_multi_frame():
    blocks = _make_block_offsets(3, 4, fill=-4)
    result = write_x265_zones_arg(blocks, duration_frames=30)
    assert result.startswith("0,29,q=")


def test_write_x265_zones_arg_mean_offset_clamped():
    # Extreme fill — offset should be clamped to QP_OFFSET_MIN/MAX.
    blocks = _make_block_offsets(2, 2, fill=QP_OFFSET_MIN)
    result = write_x265_zones_arg(blocks, duration_frames=5)
    # Parsed offset must be within the allowed band.
    offset_str = result.split("q=")[1]
    offset = int(offset_str)
    assert QP_OFFSET_MIN <= offset <= QP_OFFSET_MAX


def test_write_x265_zones_arg_zero_offset_for_neutral_saliency():
    # 0.5 saliency → 0 QP delta.
    mask = np.full((64, 64), 0.5, dtype=np.float32)
    qp_map = saliency_to_qp_map(mask, baseline_qp=28, foreground_offset=-4)
    blocks = reduce_qp_map_to_blocks(qp_map, block=X264_MB_SIDE)
    result = write_x265_zones_arg(blocks, duration_frames=1)
    offset = int(result.split("q=")[1])
    assert offset == 0


def test_augment_extra_params_with_x265_zones_appends_x265_params():
    zones = "0,29,q=-4"
    out = augment_extra_params_with_x265_zones((), zones)
    assert "-x265-params" in out
    idx = list(out).index("-x265-params")
    assert out[idx + 1] == f"zones={zones}"


def test_augment_extra_params_with_x265_zones_preserves_base():
    base = ("-x265-params", "pass=1:stats=/tmp/s")
    zones = "0,9,q=-3"
    out = augment_extra_params_with_x265_zones(base, zones)
    assert out[0] == "-x265-params"
    assert out[1] == "pass=1:stats=/tmp/s"
    assert out[2] == "-x265-params"
    assert out[3] == f"zones={zones}"


# ===========================================================================
# X265Adapter.zones_from_saliency
# ===========================================================================


def test_x265_adapter_has_supports_saliency_roi():
    a = X265Adapter()
    assert a.supports_saliency_roi is True


def test_x265_adapter_zones_from_saliency_returns_string():
    a = X265Adapter()
    blocks = _make_block_offsets(4, 5, fill=-3)
    result = a.zones_from_saliency(blocks, duration_frames=10)
    assert isinstance(result, str)
    assert result.startswith("0,9,q=")


def test_x265_adapter_supports_two_pass_unchanged():
    # ADR-0370 must NOT touch two-pass support.
    a = X265Adapter()
    assert a.supports_two_pass is True


# ===========================================================================
# LibaomAdapter.qpfile_from_saliency
# ===========================================================================


def test_augment_extra_params_with_libaom_qpfile_appends_top_level_qpfile(tmp_path):
    qpfile = tmp_path / "roi.qpfile.txt"
    qpfile.write_text("0 I 0\n", encoding="ascii")
    out = augment_extra_params_with_libaom_qpfile((), qpfile)
    assert out == ("-qpfile", str(qpfile))


def test_augment_extra_params_with_libaom_qpfile_preserves_base(tmp_path):
    qpfile = tmp_path / "roi.qpfile.txt"
    qpfile.write_text("0 I 0\n", encoding="ascii")
    base = ("-threads", "4")
    out = augment_extra_params_with_libaom_qpfile(base, qpfile)
    assert out == ("-threads", "4", "-qpfile", str(qpfile))


def test_libaom_adapter_has_supports_saliency_roi():
    a = LibaomAdapter()
    assert a.supports_qpfile is True
    assert a.supports_saliency_roi is True


def test_libaom_adapter_qpfile_from_saliency_writes_file(tmp_path):
    a = LibaomAdapter()
    blocks = _make_saliency_offsets(64, 64, block=X264_MB_SIDE, fill=-4)
    out = tmp_path / "libaom.qpfile.txt"
    result = a.qpfile_from_saliency(blocks, out, duration_frames=2)
    assert Path(result) == out
    text = out.read_text(encoding="ascii")
    assert text.startswith("0 I 0\n")
    assert "1 P 0\n" in text


def test_libaom_adapter_extra_params_unchanged():
    a = LibaomAdapter()
    assert a.extra_params() == ()


# ===========================================================================
# write_svtav1_qpoffset_map
# ===========================================================================


def test_write_svtav1_qpoffset_map_single_frame(tmp_path):
    blocks = _make_block_offsets(2, 3, fill=-3)
    out = tmp_path / "qpmap.txt"
    result = write_svtav1_qpoffset_map(blocks, out, duration_frames=1)
    assert result == out
    text = out.read_text(encoding="ascii")
    lines = [ln for ln in text.splitlines() if ln.strip()]
    # 2 rows × 3 cols.
    assert len(lines) == 2
    for line in lines:
        cols = line.split()
        assert len(cols) == 3
        assert all(int(c) == -3 for c in cols)


def test_write_svtav1_qpoffset_map_multi_frame_blank_separator(tmp_path):
    blocks = _make_block_offsets(2, 2, fill=2)
    out = tmp_path / "qpmap.txt"
    write_svtav1_qpoffset_map(blocks, out, duration_frames=3)
    text = out.read_text(encoding="ascii")
    # 3 frames × 2 rows = 6 data lines, separated by 2 blank lines.
    # Split on double-newline and count non-empty segments.
    segments = [s.strip() for s in text.split("\n\n") if s.strip()]
    assert len(segments) == 3


def test_write_svtav1_qpoffset_map_creates_parent(tmp_path):
    blocks = _make_block_offsets(1, 2, fill=0)
    out = tmp_path / "sub" / "deep" / "qpmap.txt"
    write_svtav1_qpoffset_map(blocks, out, duration_frames=1)
    assert out.exists()


def test_augment_extra_params_with_svtav1_qpmap_appends_svtav1_params(tmp_path):
    qpmap = tmp_path / "qp.txt"
    qpmap.write_text("0 0\n", encoding="ascii")
    out = augment_extra_params_with_svtav1_qpmap((), qpmap)
    assert "-svtav1-params" in out
    idx = list(out).index("-svtav1-params")
    assert out[idx + 1] == f"qp-file={qpmap}"


# ===========================================================================
# SvtAv1Adapter.qpmap_from_saliency
# ===========================================================================


def test_svtav1_adapter_has_supports_saliency_roi():
    a = SvtAv1Adapter()
    assert a.supports_saliency_roi is True


def test_svtav1_adapter_qpmap_from_saliency_writes_file(tmp_path):
    a = SvtAv1Adapter()
    # 128x128 frame at 64x64 SB granularity → 2x2 block grid.
    blocks = _make_saliency_offsets(128, 128, block=SVTAV1_SB_SIDE, fill=-4)
    out = tmp_path / "qpmap.txt"
    result = a.qpmap_from_saliency(blocks, out, duration_frames=2)
    assert Path(result) == out
    assert out.exists()
    text = out.read_text(encoding="ascii")
    assert len(text) > 0


def test_svtav1_adapter_does_not_touch_two_pass():
    # SVT-AV1 never had two_pass_args; field must stay absent/False.
    a = SvtAv1Adapter()
    # Protocol says supports_two_pass may be False or absent — we test
    # the adapter does NOT accidentally grow a truthy value.
    two_pass = getattr(a, "supports_two_pass", False)
    assert not two_pass


# ===========================================================================
# write_vvenc_roi_csv
# ===========================================================================


def test_write_vvenc_roi_csv_single_frame(tmp_path):
    blocks = _make_block_offsets(2, 3, fill=-5)
    out = tmp_path / "roi.csv"
    result = write_vvenc_roi_csv(blocks, out, duration_frames=1)
    assert result == out
    text = out.read_text(encoding="ascii")
    lines = [ln for ln in text.splitlines() if ln.strip()]
    assert len(lines) == 2
    for line in lines:
        cols = line.split(",")
        assert len(cols) == 3
        assert all(int(c) == -5 for c in cols)


def test_write_vvenc_roi_csv_multi_frame_blank_separator(tmp_path):
    blocks = _make_block_offsets(3, 2, fill=1)
    out = tmp_path / "roi.csv"
    write_vvenc_roi_csv(blocks, out, duration_frames=4)
    text = out.read_text(encoding="ascii")
    segments = [s.strip() for s in text.split("\n\n") if s.strip()]
    assert len(segments) == 4


def test_write_vvenc_roi_csv_uses_comma_separator(tmp_path):
    blocks = _make_block_offsets(1, 4, fill=2)
    out = tmp_path / "roi.csv"
    write_vvenc_roi_csv(blocks, out, duration_frames=1)
    first_line = out.read_text(encoding="ascii").splitlines()[0]
    # Must be comma-separated (not space-separated like SVT-AV1).
    assert "," in first_line
    assert " " not in first_line


def test_augment_extra_params_with_vvenc_roi_appends_vvenc_params(tmp_path):
    roi_csv = tmp_path / "roi.csv"
    roi_csv.write_text("0,0\n", encoding="ascii")
    out = augment_extra_params_with_vvenc_roi((), roi_csv)
    assert "-vvenc-params" in out
    idx = list(out).index("-vvenc-params")
    assert out[idx + 1] == f"ROIFile={roi_csv}"


def test_augment_extra_params_with_vvenc_roi_preserves_base(tmp_path):
    roi_csv = tmp_path / "roi.csv"
    roi_csv.write_text("0,0\n", encoding="ascii")
    base = ("-vvenc-params", "PerceptQPA=1")
    out = augment_extra_params_with_vvenc_roi(base, roi_csv)
    assert out[0] == "-vvenc-params"
    assert out[1] == "PerceptQPA=1"
    assert out[2] == "-vvenc-params"
    assert out[3] == f"ROIFile={roi_csv}"


# ===========================================================================
# VVenCAdapter.roi_from_saliency
# ===========================================================================


def test_vvenc_adapter_has_supports_saliency_roi():
    a = VVenCAdapter()
    assert a.supports_saliency_roi is True


def test_vvenc_adapter_roi_from_saliency_writes_csv(tmp_path):
    a = VVenCAdapter()
    # 128x128 frame at 64x64 CTU granularity → 2x2 block grid.
    blocks = _make_saliency_offsets(128, 128, block=VVENC_CTU_SIDE, fill=-4)
    out = tmp_path / "roi.csv"
    result = a.roi_from_saliency(blocks, out, duration_frames=2)
    assert Path(result) == out
    assert out.exists()
    text = out.read_text(encoding="ascii")
    # Must be comma-separated.
    assert "," in text


def test_vvenc_adapter_existing_extra_params_unaffected():
    # Adding saliency support must not change the regular extra_params surface.
    a = VVenCAdapter(perceptual_qpa=True)
    params = a.extra_params()
    assert "-vvenc-params" in params
    assert "PerceptQPA=1" in params[1]


# ===========================================================================
# Block-granularity constants
# ===========================================================================


def test_svtav1_sb_side_is_64():
    assert SVTAV1_SB_SIDE == 64


def test_vvenc_ctu_side_is_64():
    assert VVENC_CTU_SIDE == 64


def test_x264_mb_side_is_16():
    assert X264_MB_SIDE == 16
