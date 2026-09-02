# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Runtime-variant parsing for compare encoder tokens."""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.encoder_runtime import (
    parse_encoder_runtime_token,
    resolve_encoder_runtime_specs,
)


def test_parse_plain_encoder_token_uses_adapter_as_display() -> None:
    spec = parse_encoder_runtime_token("libsvtav1", ffmpeg_bin="/opt/ffmpeg-main")

    assert spec.token == "libsvtav1"
    assert spec.adapter == "libsvtav1"
    assert spec.variant == ""
    assert spec.ffmpeg_bin == "/opt/ffmpeg-main"


def test_parse_runtime_variant_keeps_base_adapter() -> None:
    spec = parse_encoder_runtime_token("libsvtav1@svt-av1-hdr")

    assert spec.token == "libsvtav1@svt-av1-hdr"
    assert spec.adapter == "libsvtav1"
    assert spec.variant == "svt-av1-hdr"


def test_resolve_encoder_runtime_specs_binds_exact_token_ffmpeg() -> None:
    specs = resolve_encoder_runtime_specs(
        ["libsvtav1", "libsvtav1@svt-av1-hdr"],
        ffmpeg_bin="/opt/ffmpeg-main",
        encoder_ffmpeg_bins=["libsvtav1@svt-av1-hdr=/opt/ffmpeg-hdr"],
    )

    by_token = {spec.token: spec for spec in specs}
    assert by_token["libsvtav1"].ffmpeg_bin == "/opt/ffmpeg-main"
    assert by_token["libsvtav1@svt-av1-hdr"].ffmpeg_bin == "/opt/ffmpeg-hdr"
    assert by_token["libsvtav1@svt-av1-hdr"].adapter == "libsvtav1"


def test_resolve_encoder_runtime_specs_rejects_unknown_binding_key() -> None:
    with pytest.raises(ValueError, match="is not in --encoders"):
        resolve_encoder_runtime_specs(
            ["libsvtav1"],
            encoder_ffmpeg_bins=["libsvtav1@svt-av1-hdr=/opt/ffmpeg-hdr"],
        )


def test_resolve_encoder_runtime_specs_rejects_duplicate_display_token() -> None:
    with pytest.raises(ValueError, match="duplicate encoder token"):
        resolve_encoder_runtime_specs(["libsvtav1@hdr", "libsvtav1@hdr"])
