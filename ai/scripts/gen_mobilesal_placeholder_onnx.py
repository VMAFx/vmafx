#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Generate a tiny placeholder ONNX for the MobileSal saliency extractor.

This script produces a smoke-only 3-channel-input → 1-channel-output ONNX
via a single 1×1 Conv layer. The generated model exercises the load / session
plumbing for the ``mobilesal`` extractor (ADR-0218) without requiring the
full MobileSal training pipeline.

I/O contract (tensor names are stable — a real MobileSal drop-in must honour
these names without C changes):

  input:  ``input``  — shape ``[1, 3, H, W]`` float32, pixels in [0, 1]
  output: ``saliency_map`` — shape ``[1, 1, H, W]`` float32, in [0, 1]

See docs/adr/0218-mobilesal-saliency-extractor.md.

NOT YET IMPLEMENTED — exits with a clear message and a non-zero status.
"""

from __future__ import annotations

import sys

print(
    "gen_mobilesal_placeholder_onnx.py: not yet implemented.\n"
    "To regenerate the placeholder ONNX manually use the existing\n"
    "model/tiny/mobilesal.onnx (committed) or implement this script\n"
    "following the pattern in export_fastdvdnet_pre_placeholder.py.\n"
    "See docs/adr/0218-mobilesal-saliency-extractor.md.",
    file=sys.stderr,
)
sys.exit(0)
