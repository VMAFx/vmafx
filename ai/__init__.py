# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Top-level ``ai`` package.

Houses the Netflix-corpus loader, feature extractor, distillation
scoring, and Lightning-driven training harness. The original
``vmaf-train`` CLI / pyproject package lives at ``ai/src/vmaf_train``;
this top-level ``ai`` package complements it with the smaller, more
focused entry points required by ADR-0242 / ADR-0203.

Sub-packages:

* :mod:`ai.data`  — Netflix-corpus loaders, libvmaf-CLI wrappers, and
  distillation-score helpers.
* :mod:`ai.train` — PyTorch / Lightning training, evaluation, and ONNX
  export entry points.

The ``ai`` package is a *namespace* surface — there are no module-level
public symbols; downstream callers ``import ai.data.netflix_loader`` etc.
directly. ``__all__`` is therefore the canonical list of *sub-packages*
guaranteed to exist under ``ai``, per ADR-0911.
"""

from __future__ import annotations

__all__ = [
    "data",
    "train",
]
