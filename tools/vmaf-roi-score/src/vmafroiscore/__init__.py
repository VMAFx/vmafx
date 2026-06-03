# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""``vmaf-roi-score`` — region-of-interest VMAF *scoring* (Option C).

Distinct surface from `core/tools/vmaf_roi.c` (ADR-0247), which is
the *encoder-steering* sidecar that emits per-CTU QP offsets. This
package is the *scoring* counterpart: it produces a saliency-weighted
combination of two VMAF runs.

Math::

    roi_vmaf = (1 - w) * vmaf_full + w * vmaf_masked

where ``vmaf_full`` is the standard full-frame VMAF and ``vmaf_masked``
is the VMAF of a saliency-masked variant of the distorted YUV (low-
saliency pixels substituted with reference pixels so they score as a
perfect match).

Option C is intentionally tool-level: it drives the existing ``vmaf``
binary twice and combines pooled scalars. It does
**not** modify libvmaf's per-pixel feature pooling, and it therefore
cannot deliver true per-pixel saliency-weighted VMAF — see
ADR-0296 for the deferred Option A roadmap.
"""

from __future__ import annotations

__version__ = "0.0.1"

__all__ = [
    "ROI_RESULT_KEYS",
    "SCHEMA_VERSION",
    "__version__",
    "blend_scores",
]

# JSON output schema version — bumped when ROIResult shape changes.
SCHEMA_VERSION = 1

# Canonical key order for the JSON payload; tests pin this.
ROI_RESULT_KEYS = (
    "schema_version",
    "vmaf_full",
    "vmaf_masked",
    "weight",
    "vmaf_roi",
    "model",
    "saliency_model",
    "reference",
    "distorted",
)


def blend_scores(vmaf_full: float, vmaf_masked: float, weight: float) -> float:
    """Combine the two pooled VMAF scalars with a saliency weight.

    ``weight`` is the saliency-masked component's contribution. ``0.0``
    returns the full-frame score unchanged; ``1.0`` returns the
    saliency-masked score unchanged.

    Raises ``ValueError`` if ``weight`` is outside ``[0, 1]`` or any
    score is non-finite. The combine math is a pure linear blend on
    Python ``float``; tests pin it.
    """
    if not (0.0 <= weight <= 1.0):
        raise ValueError(f"weight must be in [0, 1], got {weight!r}")
    for label, value in (("vmaf_full", vmaf_full), ("vmaf_masked", vmaf_masked)):
        if not _is_finite(value):
            raise ValueError(f"{label} must be finite, got {value!r}")
    return (1.0 - weight) * float(vmaf_full) + weight * float(vmaf_masked)


def _is_finite(x: float) -> bool:
    """``math.isfinite`` without importing math at module top — keeps
    the import surface minimal so the package loads cleanly even when
    the optional ``[runtime]`` deps are absent."""
    import math

    return math.isfinite(x)
