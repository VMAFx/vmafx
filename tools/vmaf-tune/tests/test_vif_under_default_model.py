# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for VIF injection and canonical-6 parsing under default model.

When scoring with a model lacking VIF (such as the default vmaf_v1.0.16_3d0h
since ADR-1168), score.build_vmaf_command must append `--feature vif` so that
all canonical-6 features (adm2, vif_scale0..3, motion2) are computed.
Under v0.6 family models (e.g. vmaf_v0.6.1), VIF is already requested natively
so `--feature vif` is omitted.

Furthermore, libvmaf registers feature aliases with option suffixes when options
are specified (e.g. integer_adm2_csf_..., integer_motion2_mmxv_18).
parse_feature_aggregates must resolve these via prefix matching so that
canonical-6 columns populate rather than emitting NaN.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

import pytest

from vmaftune import CANONICAL6_FEATURES
from vmaftune.defaultmodel import DEFAULT_MODEL
from vmaftune.score import (
    ScoreRequest,
    _model_requests_vif,
    build_vmaf_command,
    parse_feature_aggregates,
)


def test_build_vmaf_command_requests_vif_under_default_model():
    req = ScoreRequest(
        reference=Path("/tmp/ref.yuv"),
        distorted=Path("/tmp/dist.yuv"),
        width=1920,
        height=1080,
        pix_fmt="yuv420p",
        model=DEFAULT_MODEL,
    )
    cmd = build_vmaf_command(req, json_output=Path("/tmp/out.json"))
    assert "--feature" in cmd
    idx = cmd.index("--feature")
    assert cmd[idx + 1] == "vif"


def test_build_vmaf_command_omits_vif_under_v061():
    # vmaf-model-pin: test model-awareness for v0.6.1 family
    for model_str in ("vmaf_v0.6.1", "version=vmaf_v0.6.1", "vmaf_v0.6.1neg", "vmaf_4k_v0.6.1"):
        req = ScoreRequest(
            reference=Path("/tmp/ref.yuv"),
            distorted=Path("/tmp/dist.yuv"),
            width=1920,
            height=1080,
            pix_fmt="yuv420p",
            model=model_str,
        )
        cmd = build_vmaf_command(req, json_output=Path("/tmp/out.json"))
        assert "--feature" not in cmd
        assert "vif" not in cmd


def test_model_requests_vif_builtin_and_custom(tmp_path: Path):
    assert _model_requests_vif("vmaf_v0.6.1") is True
    assert _model_requests_vif("version=vmaf_v0.6.1") is True
    assert _model_requests_vif("vmaf_v0.6.1neg") is True
    assert _model_requests_vif("vmaf_b_v0.6.3") is True
    assert _model_requests_vif("vmaf_4k_v0.6.1") is True

    assert _model_requests_vif(DEFAULT_MODEL) is False
    assert _model_requests_vif("vmaf_v1.0.16_3d0h") is False
    assert _model_requests_vif("version=vmaf_v1.0.16_3d0h") is False
    assert _model_requests_vif("") is False

    # Custom model file with VIF
    model_with_vif = tmp_path / "model_with_vif.json"
    model_with_vif.write_text(
        json.dumps({"model_dict": {"feature_names": ["vif_scale0", "integer_adm2"]}}),
        encoding="utf-8",
    )
    assert _model_requests_vif(str(model_with_vif)) is True
    assert _model_requests_vif(f"path={model_with_vif}") is True

    # Custom model file without VIF
    model_no_vif = tmp_path / "model_no_vif.json"
    model_no_vif.write_text(
        json.dumps({"model_dict": {"feature_names": ["cambi", "integer_adm3"]}}),
        encoding="utf-8",
    )
    assert _model_requests_vif(str(model_no_vif)) is False
    assert _model_requests_vif(f"path={model_no_vif}") is False


def test_parse_feature_aggregates_options_suffixed_keys():
    """Libvmaf emits options-suffixed keys under v1 model + vif CLI option."""
    payload = {
        "pooled_metrics": {
            "integer_adm2_csf_2_dlmw_0.7_egl_1_min_0.5_nw_0.02": {
                "min": 0.961025,
                "max": 0.961025,
                "mean": 0.961025,
                "harmonic_mean": 0.961025,
            },
            "integer_vif_scale0": {
                "min": 0.505712,
                "max": 0.505712,
                "mean": 0.505712,
                "harmonic_mean": 0.505712,
            },
            "integer_vif_scale1": {
                "min": 0.879061,
                "max": 0.879061,
                "mean": 0.879061,
                "harmonic_mean": 0.879061,
            },
            "integer_vif_scale2": {
                "min": 0.937873,
                "max": 0.937873,
                "mean": 0.937873,
                "harmonic_mean": 0.937873,
            },
            "integer_vif_scale3": {
                "min": 0.964301,
                "max": 0.964301,
                "mean": 0.964301,
                "harmonic_mean": 0.964301,
            },
            "integer_motion2_mmxv_18": {
                "min": 1.25,
                "max": 1.25,
                "mean": 1.25,
                "harmonic_mean": 1.25,
            },
            "vmaf": {
                "mean": 89.466,
            },
        }
    }

    means, _ = parse_feature_aggregates(payload, CANONICAL6_FEATURES)
    assert means["adm2"] == pytest.approx(0.961025)
    assert means["vif_scale0"] == pytest.approx(0.505712)
    assert means["vif_scale1"] == pytest.approx(0.879061)
    assert means["vif_scale2"] == pytest.approx(0.937873)
    assert means["vif_scale3"] == pytest.approx(0.964301)
    assert means["motion2"] == pytest.approx(1.25)
    assert len(means) == 6


def test_parse_feature_aggregates_suffixed_vif_keys():
    """Verify vif_scale0..3 with custom option suffixes also resolve via prefix matching."""
    payload = {
        "pooled_metrics": {
            "integer_adm2": {"mean": 0.98},
            "integer_vif_scale0_opt_1": {"mean": 0.51},
            "integer_vif_scale1_opt_1": {"mean": 0.88},
            "integer_vif_scale2_opt_1": {"mean": 0.94},
            "integer_vif_scale3_opt_1": {"mean": 0.97},
            "integer_motion2": {"mean": 2.5},
        }
    }
    means, _ = parse_feature_aggregates(payload, CANONICAL6_FEATURES)
    assert means["adm2"] == pytest.approx(0.98)
    assert means["vif_scale0"] == pytest.approx(0.51)
    assert means["vif_scale1"] == pytest.approx(0.88)
    assert means["vif_scale2"] == pytest.approx(0.94)
    assert means["vif_scale3"] == pytest.approx(0.97)
    assert means["motion2"] == pytest.approx(2.5)
