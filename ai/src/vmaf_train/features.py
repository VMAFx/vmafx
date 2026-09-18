# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Feature vector contract shared between datamodule and audit tools.

Kept in its own module so lightweight callers (audit, MCP tools, op-allowlist
check) don't pay the torch/lightning import cost just to know the column
count or names.
"""

from __future__ import annotations

FEATURE_COLUMNS: tuple[str, ...] = (
    "adm2",
    "vif_scale0",
    "vif_scale1",
    "vif_scale2",
    "vif_scale3",
    "motion2",
)
