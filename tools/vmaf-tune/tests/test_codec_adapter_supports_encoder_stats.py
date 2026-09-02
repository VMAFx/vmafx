# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Every adapter declares the ``supports_encoder_stats`` flag (ADR-0332).

Hardware encoders (NVENC / AMF / QSV / VideoToolbox) and AV1 software
encoders (libaom-av1, libsvtav1, libvvenc) opt out. Software encoders
that emit a parseable pass-1 stats file (libx264, libx265) opt in.
"""

from __future__ import annotations

import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.codec_adapters import get_adapter, known_codecs

# Codecs that the v1 stats-capture path supports.
_OPT_IN: frozenset[str] = frozenset({"libx264", "libx265"})


def test_every_adapter_declares_supports_encoder_stats():
    for name in known_codecs():
        adapter = get_adapter(name)
        assert hasattr(
            adapter, "supports_encoder_stats"
        ), f"adapter {name!r} missing supports_encoder_stats flag"
        assert isinstance(adapter.supports_encoder_stats, bool)


def test_x264_and_x265_opt_in():
    for name in _OPT_IN:
        adapter = get_adapter(name)
        assert (
            adapter.supports_encoder_stats is True
        ), f"adapter {name!r} should declare supports_encoder_stats=True"


def test_hardware_and_av1_codecs_opt_out():
    expected_opt_out = set(known_codecs()) - _OPT_IN
    for name in expected_opt_out:
        adapter = get_adapter(name)
        assert (
            adapter.supports_encoder_stats is False
        ), f"adapter {name!r} should declare supports_encoder_stats=False"
