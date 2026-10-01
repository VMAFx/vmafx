# SPDX-License-Identifier: BSD-2-Clause-Patent
from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class RdPoint:
    rate: float
    metric: float
