# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tiny-AI training entry points for the Netflix corpus.

* :mod:`ai.train.dataset`              — PyTorch :class:`Dataset` over
  per-frame feature vectors with ``vmaf_v0.6.1`` distillation targets.
* :mod:`ai.train.konvid_pair_dataset`  — KoNViD-1k paired dataset loader
  used by the combined-objective trainer.
* :mod:`ai.train.eval`                 — PLCC / SROCC / KROCC / RMSE
  harness plus inference-latency timing.
* :mod:`ai.train.train`                — Lightning-driven training entry
  point with ONNX export per epoch.
* :mod:`ai.train.train_combined`       — Combined distillation +
  KoNViD-pair objective trainer.
* :mod:`ai.train.qat`                  — Quantization-aware training
  helpers for int8 ONNX export.

ADR-0203 documents the architecture-, split-, and caching-strategy
decisions; see ``docs/ai/training.md`` for the user-facing guide.
"""

from __future__ import annotations

__all__ = [
    "dataset",
    "eval",
    "konvid_pair_dataset",
    "qat",
    "train",
    "train_combined",
]
