#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Train the codec-aware FR regressor v4.

v4 extends the v3 recipe with additional corpus sources and an updated
feature set.  It is the next planned rung in the FR regressor ladder
after v3 (ADR-0235).

See docs/research/0091-partial-integration-audit-2026-05-08.md for
the v4 scope and tracking context.

NOT YET IMPLEMENTED — exits with a clear message and a non-zero status.
"""

from __future__ import annotations

import sys

print(
    "train_fr_regressor_v4.py: not yet implemented.\n"
    "See docs/research/0091-partial-integration-audit-2026-05-08.md\n"
    "for the v4 scope.  Implement following the pattern in\n"
    "train_fr_regressor_v3.py (ADR-0235).",
    file=sys.stderr,
)
sys.exit(0)
