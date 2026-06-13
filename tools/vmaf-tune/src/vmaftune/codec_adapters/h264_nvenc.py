# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""NVIDIA NVENC H.264 codec adapter.

Hardware-accelerated AVC encoder exposed through FFmpeg's ``h264_nvenc``
encoder. Uses the constant-quantizer (``-cq``) knob as the closest
analogue to libx264's CRF; preset names map onto NVENC's ``p1``..``p7``
levels via the shared mnemonic table in ``_nvenc_common``.

All encode/validate/probe logic lives in
:class:`~._nvenc_common.BaseNvencAdapter`; this file pins only the
codec-identity fields.
"""

from __future__ import annotations

import dataclasses

from ._nvenc_common import BaseNvencAdapter


@dataclasses.dataclass(frozen=True)
class H264NvencAdapter(BaseNvencAdapter):
    """NVIDIA NVENC H.264 single-pass CQ adapter."""

    name: str = "h264_nvenc"
    encoder: str = "h264_nvenc"
