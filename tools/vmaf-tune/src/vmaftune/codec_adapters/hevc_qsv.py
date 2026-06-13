# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Intel QSV HEVC codec adapter (`hevc_qsv`).

ICQ-mode single-pass encodes via ``-global_quality``; same preset
vocabulary as ``h264_qsv``.

Hardware availability: 7th-gen+ Intel iGPU (Kaby Lake and newer; HEVC
10-bit needs Tiger Lake / 11th-gen+) or Arc / Battlemage discrete GPUs.

All encode/validate/probe logic lives in
:class:`~._qsv_common.BaseQsvAdapter`; this file pins only the
codec-identity fields.
"""

from __future__ import annotations

import dataclasses

from ._qsv_common import BaseQsvAdapter


@dataclasses.dataclass(frozen=True)
class HevcQsvAdapter(BaseQsvAdapter):
    """Intel QSV HEVC single-pass ICQ adapter."""

    name: str = "hevc_qsv"
    encoder: str = "hevc_qsv"
