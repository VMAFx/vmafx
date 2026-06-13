# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Intel QSV AV1 codec adapter (`av1_qsv`).

ICQ-mode single-pass encodes via ``-global_quality``; same preset
vocabulary as the H.264 / HEVC QSV adapters.

Hardware availability is narrower than the H.264 / HEVC encoders —
AV1 fixed-function encode requires either a 12th-gen+ Intel iGPU
(Alder Lake-N and newer with the AV1 block) or Arc / Battlemage
discrete GPUs. Older silicon will register the encoder but fail at
runtime with a libmfx ``MFX_ERR_UNSUPPORTED`` style diagnostic; the
``ffmpeg_supports_encoder`` probe in ``_qsv_common`` catches the
build-time variant of that mismatch.

All encode/validate/probe logic lives in
:class:`~._qsv_common.BaseQsvAdapter`; this file pins only the
codec-identity fields.
"""

from __future__ import annotations

import dataclasses

from ._qsv_common import BaseQsvAdapter


@dataclasses.dataclass(frozen=True)
class Av1QsvAdapter(BaseQsvAdapter):
    """Intel QSV AV1 single-pass ICQ adapter."""

    name: str = "av1_qsv"
    encoder: str = "av1_qsv"
