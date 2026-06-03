# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""AMD AMF H.264 codec adapter (``h264_amf``).

Hardware-accelerated H.264 encoding through AMD's Advanced Media
Framework. Requires an AMD GPU and an ffmpeg build with
``--enable-amf``. See ``_amf_common`` for the shared 7-into-3
preset compression and the ``-rc cqp`` rate-control rationale.
"""

from __future__ import annotations

import dataclasses

from ._amf_common import _AMFAdapterBase


@dataclasses.dataclass(frozen=True)
class H264AMFAdapter(_AMFAdapterBase):
    """AMD AMF H.264 single-pass constant-QP adapter."""

    name: str = "h264_amf"
    encoder: str = "h264_amf"


__all__ = ["H264AMFAdapter"]
