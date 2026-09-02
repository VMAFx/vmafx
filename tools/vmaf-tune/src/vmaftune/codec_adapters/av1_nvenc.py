# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""NVIDIA NVENC AV1 codec adapter.

Hardware-accelerated AV1 encoder exposed through FFmpeg's
``av1_nvenc`` encoder. Available only on NVIDIA Ada Lovelace
generation hardware (RTX 40-series and the L40 / L4 server parts) and
later — older GPUs return "Encoder not found" at FFmpeg invocation
time. Adapter shape is identical to ``h264_nvenc`` / ``hevc_nvenc``;
the hardware-availability check belongs at run time, not in the
adapter contract.

All encode/validate/probe logic lives in
:class:`~._nvenc_common.BaseNvencAdapter`; this file pins only the
codec-identity fields.
"""

from __future__ import annotations

import dataclasses

from ._nvenc_common import BaseNvencAdapter


@dataclasses.dataclass(frozen=True)
class Av1NvencAdapter(BaseNvencAdapter):
    """NVIDIA NVENC AV1 single-pass CQ adapter (Ada+ hardware only)."""

    name: str = "av1_nvenc"
    encoder: str = "av1_nvenc"
