# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Datasets, feature dumps, frame loaders, and deterministic splits.

Sub-modules:

* :mod:`vmaf_train.data.datasets`       — PyTorch dataset adapters.
* :mod:`vmaf_train.data.feature_dump`   — Parquet feature-dump I/O.
* :mod:`vmaf_train.data.frame_dataset`  — frame-level dataset wrappers.
* :mod:`vmaf_train.data.frame_loader`   — raw YUV / decoded-frame loaders.
* :mod:`vmaf_train.data.manifest_scan`  — manifest discovery helpers.
* :mod:`vmaf_train.data.splits`         — deterministic train/val/test
  split allocation (hash-bucketed; see :class:`Splits`).

The package itself exposes no module-level symbols; downstream callers
import from sub-modules directly. ``__all__`` lists the canonical
sub-module set per ADR-0911.
"""

__all__ = [
    "datasets",
    "feature_dump",
    "frame_dataset",
    "frame_loader",
    "manifest_scan",
    "splits",
]
