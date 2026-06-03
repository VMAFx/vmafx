# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""AMD AMF AV1 codec adapter (``av1_amf``).

Hardware-accelerated AV1 encoding through AMD's Advanced Media
Framework. AV1 is RDNA3-only — Radeon RX 7000 series or newer; on
older silicon the encoder will not register and ``ensure_amf_available``
will reject the build.

Requires an AMD GPU and an ffmpeg build with ``--enable-amf``. See
``_amf_common`` for the shared 7-into-3 preset compression and the
``-rc cqp`` rate-control rationale.
"""

from __future__ import annotations

import dataclasses

from ._amf_common import _AMFAdapterBase


@dataclasses.dataclass(frozen=True)
class AV1AMFAdapter(_AMFAdapterBase):
    """AMD AMF AV1 single-pass constant-QP adapter (RDNA3+ only)."""

    name: str = "av1_amf"
    encoder: str = "av1_amf"


__all__ = ["AV1AMFAdapter"]
