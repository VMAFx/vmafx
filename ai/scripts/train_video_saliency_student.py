#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Train the video-temporal saliency student (Phase 2 of ADR-0396).

The student is a TinyU-Net with a learned per-channel EMA gate on the
bottleneck, distilled from UNISAL (Apache-2.0, MobileNetV2 + Bypass-RNN)
on DHF1K (CC BY 4.0).  It shares the same I/O contract as
``saliency_student_v1`` (ADR-0286) plus an optional bottleneck-state input.

See docs/adr/0396-video-saliency-extension.md (Phase 2 subsection).

NOT YET IMPLEMENTED — exits with a clear message and a non-zero status.
"""

from __future__ import annotations

import sys

print(
    "train_video_saliency_student.py: not yet implemented.\n"
    "This is the Phase-2 video saliency student trainer from ADR-0396.\n"
    "Phase 1 (temporal aggregator in vmaf-tune) is already implemented.\n"
    "See docs/adr/0396-video-saliency-extension.md for the full roadmap.",
    file=sys.stderr,
)
sys.exit(0)
