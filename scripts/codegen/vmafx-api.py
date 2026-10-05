#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Entry point of the VMAFx API generator; see vmafx_api/cli.py (ADR-1852)."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from vmafx_api.cli import main

if __name__ == "__main__":
    raise SystemExit(main())
