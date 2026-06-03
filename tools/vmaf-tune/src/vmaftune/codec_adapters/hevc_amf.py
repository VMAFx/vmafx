# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""AMD AMF HEVC codec adapter (``hevc_amf``).

Hardware-accelerated HEVC / H.265 encoding through AMD's Advanced
Media Framework. Requires an AMD GPU and an ffmpeg build with
``--enable-amf``. See ``_amf_common`` for the shared 7-into-3
preset compression and the ``-rc cqp`` rate-control rationale.
"""

from __future__ import annotations

import dataclasses

from ._amf_common import _AMFAdapterBase


@dataclasses.dataclass(frozen=True)
class HEVCAMFAdapter(_AMFAdapterBase):
    """AMD AMF HEVC single-pass constant-QP adapter."""

    name: str = "hevc_amf"
    encoder: str = "hevc_amf"


__all__ = ["HEVCAMFAdapter"]
