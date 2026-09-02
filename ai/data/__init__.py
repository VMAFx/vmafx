# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Data loaders, feature extractors, and ground-truth scoring for tiny-AI.

The :mod:`ai.data` package complements the existing ``vmaf_train.data``
package (under ``ai/src/vmaf_train/data``) with concrete entry points for
the **Netflix VMAF training corpus** described in
`docs/ai/training-data.md` and ADR-0242 / ADR-0203. Modules:

* :mod:`ai.data.netflix_loader` — pair distorted YUVs with reference YUVs
  by parsing the ``<source>_<quality>_<height>_<bitrate>.yuv`` ladder
  convention.
* :mod:`ai.data.feature_extractor` — wrap the libvmaf CLI (``vmaf``) to
  pull per-frame ``adm2``, ``vif_scale0..3``, ``motion2`` features.
* :mod:`ai.data.scores` — distill ``vmaf_v0.6.1`` scores so the tiny model
  can be trained without paywalled MOS labels.
"""

from __future__ import annotations

__all__ = [
    "feature_extractor",
    "netflix_loader",
    "scores",
]
