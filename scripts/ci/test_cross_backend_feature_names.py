#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for cross-backend CI extractor-name resolution."""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import cross_backend_parity_gate as parity_gate
import cross_backend_vif_diff as vif_diff


def test_parity_gate_feature_extractor_names() -> None:
    assert parity_gate.feature_extractor_name("adm", "cpu") == "adm"
    assert parity_gate.feature_extractor_name("adm", "cuda") == "adm_cuda"
    assert parity_gate.feature_extractor_name("adm", "sycl") == "adm_sycl"


def test_vif_diff_feature_extractor_names() -> None:
    assert vif_diff.feature_extractor_name("adm", None) == "adm"
    assert vif_diff.feature_extractor_name("adm", "cuda") == "adm_cuda"
    assert vif_diff.feature_extractor_name("adm", "sycl") == "adm_sycl"


def test_feature_option_aliases_keep_backend_suffixes() -> None:
    assert (
        parity_gate.feature_extractor_name("float_ms_ssim_lcs", "cuda")
        == "float_ms_ssim_cuda=enable_lcs=true"
    )
    assert (
        vif_diff.feature_extractor_name("float_ms_ssim_lcs", "cuda")
        == "float_ms_ssim_cuda=enable_lcs=true"
    )
