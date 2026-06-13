# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""NVIDIA NVENC HEVC codec adapter.

Hardware-accelerated H.265 encoder exposed through FFmpeg's
``hevc_nvenc`` encoder. Mirrors the H.264 NVENC adapter shape: shared
preset table, shared CQ window, shared validation. The only
distinguishing field is the FFmpeg encoder name.

All encode/validate/probe logic lives in
:class:`~._nvenc_common.BaseNvencAdapter`; this file pins only the
codec-identity fields.
"""

from __future__ import annotations

import dataclasses

from ._nvenc_common import BaseNvencAdapter


@dataclasses.dataclass(frozen=True)
class HevcNvencAdapter(BaseNvencAdapter):
    """NVIDIA NVENC HEVC single-pass CQ adapter."""

    name: str = "hevc_nvenc"
    encoder: str = "hevc_nvenc"
