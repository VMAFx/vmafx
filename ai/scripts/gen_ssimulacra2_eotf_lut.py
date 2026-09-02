#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Generate the SSIMULACRA2 EOTF look-up table baked into the extractor.

The LUT maps 10-bit PQ-encoded values to linear light for SSIMULACRA2's
HDR path. It is computed offline and committed to the tree so the C
extractor can load it at runtime without depending on a floating-point
math library call per pixel.

See docs/adr/0164-ssimulacra2-snapshot-gate.md.

NOT YET IMPLEMENTED — exits with a clear message and a non-zero status.
"""

from __future__ import annotations

import sys

print(
    "gen_ssimulacra2_eotf_lut.py: not yet implemented.\n"
    "The EOTF LUT is generated offline and baked into the extractor.\n"
    "See docs/adr/0164-ssimulacra2-snapshot-gate.md for the\n"
    "generation procedure and ADR context.",
    file=sys.stderr,
)
sys.exit(1)
