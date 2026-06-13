# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Intel QSV H.264 codec adapter (`h264_qsv`).

ICQ-mode single-pass encodes via ``-global_quality``; the seven QSV
presets share the same names as x264's medium-and-down subset.

Hardware availability: 7th-gen+ Intel iGPU (Kaby Lake and newer) or
Arc / Battlemage discrete GPUs. ``ffmpeg`` must be built with libmfx
or VPL support — probe via ``_qsv_common.ffmpeg_supports_encoder``.

All encode/validate/probe logic lives in
:class:`~._qsv_common.BaseQsvAdapter`; this file pins only the
codec-identity fields.
"""

from __future__ import annotations

import dataclasses

from ._qsv_common import BaseQsvAdapter


@dataclasses.dataclass(frozen=True)
class H264QsvAdapter(BaseQsvAdapter):
    """Intel QSV H.264 single-pass ICQ adapter."""

    name: str = "h264_qsv"
    encoder: str = "h264_qsv"
