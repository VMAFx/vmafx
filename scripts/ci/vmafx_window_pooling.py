# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Offline pooling of per-frame scores with the engine's arithmetic.

One implementation for every test that holds window scores (the live
harness, the FFmpeg `vmafx` filter, the GStreamer `vmafx` element) against
the CLI's per-frame scores of the same frames: running sums in frame order,
no reassociation, as ``pool_accumulate`` / ``pool_reduce`` /
``vmaf_percentile`` in ``core/src/libvmaf.c`` and ``core/src/percentile.h``
compute them. Slot ``p`` of :func:`pooled` is ``VmafxPool`` ``p``.
"""

from __future__ import annotations

import math

PERCENTILES = {5: 50.0, 6: 5.0, 7: 10.0, 8: 20.0}  # VmafxPool -> percentile
POOL_NAMES = ("min", "max", "mean", "harmonic_mean", "median", "perc5", "perc10", "perc20")


def cli_name(target: str) -> str:
    """The CLI's metric key of a collector feature name."""
    prefix, suffix = "VMAF_integer_feature_", "_score"
    if target.startswith(prefix) and target.endswith(suffix):
        return "integer_" + target[len(prefix) : -len(suffix)]
    return target


def percentile(scores: list[float], perc: float) -> float:
    """vmaf_percentile(): sorted scores, linear interpolation, C's order."""
    ordered = sorted(scores)
    p = perc * (len(ordered) - 1) / 100.0
    low, high = math.floor(p), math.ceil(p)
    if low == high:
        return ordered[low]
    return ordered[low] * (high - p) + ordered[high] * (p - low)


def pooled(scores: list[float]) -> list[float]:
    """The value of every VmafxPool slot 1..8 (slot 0 unused) as the engine pools."""
    total = 0.0
    inverse = 0.0
    for score in scores:
        total += score
        inverse += 1.0 / (score + 1.0)
    count = float(len(scores))
    values = [0.0, min(scores), max(scores), total / count, count / inverse - 1.0]
    values += [percentile(scores, PERCENTILES[p]) for p in range(5, 9)]
    return values


def pooled_by_name(scores: list[float]) -> dict[str, float]:
    """:func:`pooled` keyed by the pool option's value names (``mean``, ``perc5`` ...)."""
    values = pooled(scores)
    return {name: values[slot] for slot, name in enumerate(POOL_NAMES, start=1)}
