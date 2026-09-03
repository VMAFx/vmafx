# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Resolution-aware VMAF model selection + per-resolution CRF offsets.

Background — VMAF ships two production-grade pooled-mean models in this
fork's ``model/`` tree:

- ``vmaf_v0.6.1.json`` — trained on a 1080p-display viewing setup; the
  default for HD content.
- ``vmaf_4k_v0.6.1.json`` — re-fit for a 4K display setup; recommended
  by Netflix for any encode whose target display is UHD.

The wrong model on the wrong resolution biases scores by several VMAF
points (4K content scored against the 1080p model under-counts spatial
detail; 1080p content scored against the 4K model over-counts coding
artefacts). Phase A of ``vmaf-tune`` ran every cell against one fixed
model — fine for a single-resolution sweep, lossy for a mixed-ladder
corpus.

This module defines the decision rule:

- height ≥ 2160 → 4K model
- otherwise → 1080p model (the fork has no 720p / SD model; 1080p is
  the canonical fallback and matches Netflix's published guidance).

It also exposes a per-resolution CRF offset hook: when bisecting an ABR
ladder for a flat target VMAF, the rate-distortion shift across
resolution rungs is large enough that a single CRF anchor under-shoots
the bottom rungs and over-shoots the top. The offset returned here is a
small integer the search layer can apply when seeding bisect bounds.

The offsets are defaults, tuned from public AV1 / x264 ABR ladder
research; see ``docs/research/0064-vmaf-tune-resolution-aware.md`` for
provenance. They are intentionally conservative — Phase B / C / D will
learn per-codec offsets from real corpora and override these.
"""

from __future__ import annotations

from pathlib import Path

from .defaultmodel import DEFAULT_MODEL, DEFAULT_MODEL_NEG

# -----------------------------------------------------------------------------
# Decision thresholds
# -----------------------------------------------------------------------------

# Height (lines) at and above which the 4K model is selected. 2160 is the
# UHD-1 standard and matches Netflix's own model-selection guidance.
_HEIGHT_4K_THRESHOLD = 2160

# Height below which the 1080p model is the canonical fallback. Anything
# 720p / SD / sub-SD also routes here — the fork has no 720p model and
# Netflix's published recommendation is to use the 1080p model for all
# sub-2160p content.
_HEIGHT_1080P_FALLBACK = 0

# Model identifiers mirror libvmaf's `--model version=` vocabulary so the
# strings flow straight through `score.py`'s ScoreRequest.model field.
MODEL_1080P = DEFAULT_MODEL
MODEL_4K = "vmaf_4k_v0.6.1"

# NEG (No Enhancement Gain) model variants — resist sharpening-based score
# inflation. Use for codec A vs. B comparisons, not production monitoring.
# See docs/metrics/vmaf-neg.md for when to use NEG and when not to.
MODEL_1080P_NEG = DEFAULT_MODEL_NEG
MODEL_4K_NEG = "vmaf_4k_v0.6.1neg"


def _project_model_dir() -> Path:
    """Locate the in-tree ``model/`` directory.

    Walks up from this file until it finds the repo root containing a
    ``model/`` sibling. Returns the resolved Path. Used only by tests
    that want to verify the JSON file actually exists; production
    callers consume the version-string return of :func:`select_vmaf_model_version`.
    """
    here = Path(__file__).resolve()
    for parent in here.parents:
        candidate = parent / "model"
        if candidate.is_dir() and (candidate / "vmaf_v0.6.1.json").exists():
            return candidate
    # Fallback — not fatal; tests that need the path will assert.
    return Path("model")


def select_vmaf_model_version(width: int, height: int) -> str:
    """Return the libvmaf ``--model version=`` string for a given encode.

    Decision rule (see module docstring for justification):

    - ``height >= 2160`` → ``vmaf_4k_v0.6.1``
    - else → ``vmaf_v0.6.1``

    ``width`` is accepted for API symmetry with future anamorphic /
    aspect-ratio aware extensions; the current rule is height-only.
    """
    if width <= 0 or height <= 0:
        raise ValueError(f"resolution must be positive (got width={width}, height={height})")
    if height >= _HEIGHT_4K_THRESHOLD:
        return MODEL_4K
    return MODEL_1080P


def neg_model_for(model_version: str) -> str:
    """Return the NEG variant of a standard VMAF model version string.

    Maps the two supported production models to their NEG counterparts:

    - ``"vmaf_v0.6.1"``     → ``"vmaf_v0.6.1neg"``
    - ``"vmaf_4k_v0.6.1"``  → ``"vmaf_4k_v0.6.1neg"``

    Any string already ending in ``"neg"`` is returned unchanged so the
    function is idempotent. Unknown models are returned unchanged with a
    ``"neg"`` suffix appended, which lets the libvmaf CLI surface the
    missing-model error rather than silently using the wrong model.

    Args:
        model_version: Bare model version string as used in
            :class:`~vmaftune.score.ScoreRequest` (e.g.
            ``"vmaf_v0.6.1"``, ``"vmaf_4k_v0.6.1"``).  Pre-formatted
            ``key=value`` strings (``"path=/…"`` / ``"version=…"``) are
            returned unchanged — they are model-path overrides, not
            version identifiers.

    Returns:
        NEG variant string suitable for passing as
        ``ScoreRequest.model``.
    """
    # Pre-formatted path= / version= overrides — do not touch.
    if "=" in model_version:
        return model_version
    # Already NEG — idempotent.
    if model_version.endswith("neg"):
        return model_version
    _NEG_MAP = {
        MODEL_1080P: MODEL_1080P_NEG,
        MODEL_4K: MODEL_4K_NEG,
    }
    if model_version in _NEG_MAP:
        return _NEG_MAP[model_version]
    # Unknown model — append suffix so libvmaf surfaces a clear error.
    return model_version + "neg"


def select_vmaf_model(width: int, height: int) -> Path:
    """Return the in-tree JSON path for the resolution-appropriate model.

    Thin wrapper around :func:`select_vmaf_model_version` that resolves
    the version string to a concrete file under the project's
    ``model/`` directory. Useful for callers that want to verify the
    model exists before kicking off a long sweep.
    """
    version = select_vmaf_model_version(width, height)
    return _project_model_dir() / f"{version}.json"


def crf_offset_for_resolution(width: int, height: int) -> int:
    """Return the CRF offset (integer) for a given encode resolution.

    The convention: 1080p is the baseline (offset 0). Higher resolutions
    receive a *negative* offset (i.e. lower CRF, higher quality bits)
    because the same nominal CRF on 4K visibly under-shoots a flat-VMAF
    target compared to 1080p under typical x264 / x265 / AV1 RDO. Lower
    resolutions receive a *positive* offset.

    Defaults shipped with this module:

    - ``height >= 2160``  → ``-2`` (4K under-shoots at parity CRF).
    - ``height >= 1080``  → ``0``  (baseline).
    - ``height >= 720``   → ``+2`` (HD over-shoots at parity CRF).
    - ``height < 720``    → ``+4`` (SD / sub-SD).

    Width is accepted for API symmetry. See module docstring for the
    research provenance and Phase B/C/D plan to learn these per-codec.
    """
    if width <= 0 or height <= 0:
        raise ValueError(f"resolution must be positive (got width={width}, height={height})")
    if height >= _HEIGHT_4K_THRESHOLD:
        return -2
    if height >= 1080:
        return 0
    if height >= 720:
        return 2
    return 4


__all__ = [
    "MODEL_4K",
    "MODEL_4K_NEG",
    "MODEL_1080P",
    "MODEL_1080P_NEG",
    "crf_offset_for_resolution",
    "neg_model_for",
    "select_vmaf_model",
    "select_vmaf_model_version",
]
