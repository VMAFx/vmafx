# Copyright 2026 Lusoris
"""Cover the HDR-aware + HFR-aware feature-option emission added to
``ai/scripts/extract_k150k_features.py``.

Closes Issue #837 (BBC 50p HFR motion under-prediction) + the parallel
CAMBI HDR-EOTF gap surfaced by lawrence's 2026-05-15 review of the live
CHUG extraction. Both gaps share the same root cause: the extractor
was passing ``--feature <name>`` plain instead of using libvmaf's
``--feature <name>=k=v:k=v`` syntax to forward HDR-aware + fps-aware
per-feature options."""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "ai" / "scripts" / "extract_k150k_features.py"

# ai/scripts/ is not a package; load the module by path.
# Return the cached instance if already in sys.modules so that module-level
# imports in sibling test files (e.g. test_extract_k150k_perf.py) always
# patch the same object that _process_clip references as its globals.
if "extract_k150k_features" in sys.modules:
    ek = sys.modules["extract_k150k_features"]
else:
    spec = importlib.util.spec_from_file_location("extract_k150k_features", SCRIPT)
    assert spec is not None and spec.loader is not None
    ek = importlib.util.module_from_spec(spec)
    sys.modules["extract_k150k_features"] = ek
    spec.loader.exec_module(ek)


# ---------------------------------------------------------------------------
# _is_hdr_source
# ---------------------------------------------------------------------------


def test_is_hdr_source_pq_10bit():
    assert ek._is_hdr_source(
        "yuv420p10le", {"color_transfer": "smpte2084", "color_primaries": "bt2020"}
    )


def test_is_hdr_source_hlg_10bit():
    assert ek._is_hdr_source(
        "yuv420p10le", {"color_transfer": "arib-std-b67", "color_primaries": "bt2020"}
    )


def test_is_hdr_source_bt2020_primaries_only():
    # When transfer is missing but primaries are BT.2020 (and bitdepth ≥ 10),
    # treat as HDR. Better to over-apply HDR options than under-apply.
    assert ek._is_hdr_source("yuv420p10le", {"color_transfer": "", "color_primaries": "bt2020"})


def test_is_hdr_source_8bit_rejects():
    # 8-bit can't be HDR regardless of metadata.
    assert not ek._is_hdr_source(
        "yuv420p", {"color_transfer": "smpte2084", "color_primaries": "bt2020"}
    )


def test_is_hdr_source_10bit_sdr_rejects():
    # 10-bit BT.709 is SDR.
    assert not ek._is_hdr_source(
        "yuv420p10le", {"color_transfer": "bt709", "color_primaries": "bt709"}
    )


def test_is_hdr_source_missing_metadata_rejects():
    # Missing metadata fails-safe to SDR.
    assert not ek._is_hdr_source("yuv420p10le", {"color_transfer": "", "color_primaries": ""})


# ---------------------------------------------------------------------------
# _parse_fps + _motion_fps_weight
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "fps_str,expected",
    [
        ("30/1", 30.0),
        ("60/1", 60.0),
        ("120/1", 120.0),
        ("30000/1001", pytest.approx(29.97, abs=0.01)),
        ("60000/1001", pytest.approx(59.94, abs=0.01)),
        ("25", 25.0),
        ("50.0", 50.0),
        ("0/1", 0.0),
        ("garbage", 0.0),
    ],
)
def test_parse_fps(fps_str, expected):
    assert ek._parse_fps(fps_str) == expected


@pytest.mark.parametrize(
    "fps,expected",
    [
        # In the [24, 32] band — no correction (1.0).
        (24.0, 1.0),
        (25.0, 1.0),
        (29.97, 1.0),
        (30.0, 1.0),
        (32.0, 1.0),
        # Above 32 → 30/fps.
        (50.0, 0.6),
        (59.94, pytest.approx(30.0 / 59.94, abs=0.001)),
        (60.0, 0.5),
        (120.0, 0.25),
        # Below 24 → 30/fps (clamped).
        (15.0, 2.0),
        # Clamp boundaries.
        (10.0, 3.0),
        (8.0, 3.75),
        # Beyond 120, clamp at 0.25.
        (240.0, 0.25),
        # Bad fps → no correction.
        (0.0, 1.0),
        (-1.0, 1.0),
    ],
)
def test_motion_fps_weight(fps, expected):
    assert ek._motion_fps_weight(fps) == expected


# ---------------------------------------------------------------------------
# _feature_arg
# ---------------------------------------------------------------------------


def test_feature_arg_sdr_30fps_is_bare_name():
    """SDR + 30 fps → no options; preserves pre-fix behaviour."""
    assert ek._feature_arg("cambi", is_hdr=False, motion_fps_weight=1.0) == "cambi"
    assert ek._feature_arg("float_ms_ssim", is_hdr=False, motion_fps_weight=1.0) == "float_ms_ssim"
    assert ek._feature_arg("motion", is_hdr=False, motion_fps_weight=1.0) == "motion"
    assert ek._feature_arg("vif", is_hdr=False, motion_fps_weight=1.0) == "vif"


def test_feature_arg_hdr_cambi_gets_eotf_pq_and_full_ref():
    """CPU CAMBI accepts both ``eotf`` and ``full_ref``."""
    arg = ek._feature_arg("cambi", is_hdr=True, motion_fps_weight=1.0)
    assert arg.startswith("cambi=")
    assert "eotf=pq" in arg
    assert "full_ref=true" in arg


def test_feature_arg_hdr_cambi_cuda_gets_eotf_only():
    """``cambi_cuda`` exposes ``eotf`` but not ``full_ref`` — the
    whitelist must drop the unsupported option silently."""
    arg = ek._feature_arg("cambi_cuda", is_hdr=True, motion_fps_weight=1.0)
    assert arg == "cambi_cuda=eotf=pq"


def test_feature_arg_hdr_ms_ssim_cpu_only_gets_enable_db_false():
    """CPU ``float_ms_ssim`` accepts ``enable_db``; the CUDA twin
    doesn't expose it, so the CUDA arg drops back to bare name."""
    assert (
        ek._feature_arg("float_ms_ssim", is_hdr=True, motion_fps_weight=1.0)
        == "float_ms_ssim=enable_db=false"
    )
    assert (
        ek._feature_arg("float_ms_ssim_cuda", is_hdr=True, motion_fps_weight=1.0)
        == "float_ms_ssim_cuda"
    )


def test_feature_arg_hfr_60fps_motion_gets_fps_weight():
    # 60 fps → motion_fps_weight = 0.5.
    arg = ek._feature_arg("motion", is_hdr=False, motion_fps_weight=0.5)
    assert arg == "motion=motion_fps_weight=0.5000"


def test_feature_arg_hfr_60fps_motion_v2_cuda_gets_fps_weight():
    arg = ek._feature_arg("motion_v2_cuda", is_hdr=False, motion_fps_weight=0.5)
    assert arg == "motion_v2_cuda=motion_fps_weight=0.5000"


def test_feature_arg_hdr_hfr_combo_cambi():
    """HDR + HFR: CAMBI gets its HDR options; motion_fps_weight only
    applies to motion features, not CAMBI."""
    arg = ek._feature_arg("cambi", is_hdr=True, motion_fps_weight=0.5)
    assert "eotf=pq" in arg
    assert "full_ref=true" in arg
    assert "motion_fps_weight" not in arg


def test_feature_arg_hdr_hfr_combo_motion():
    """Motion gets fps_weight; HDR-only options don't apply to motion."""
    arg = ek._feature_arg("motion_v2", is_hdr=True, motion_fps_weight=0.25)
    assert arg == "motion_v2=motion_fps_weight=0.2500"
    assert "eotf=pq" not in arg


def test_feature_arg_unrelated_feature_unaffected():
    """vif / psnr / etc. don't grow options even when HDR + HFR."""
    for name in ("vif", "vif_cuda", "psnr", "psnr_cuda", "ciede", "ssimulacra2"):
        assert ek._feature_arg(name, is_hdr=True, motion_fps_weight=0.5) == name
