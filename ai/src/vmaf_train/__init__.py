# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""``vmaf-train`` — train, export, and register tiny perceptual-quality models for libvmaf.

This is the installable Python package (``pyproject.toml`` at
``ai/pyproject.toml``); it exposes the ``vmaf-train`` console script via
:mod:`vmaf_train.cli`. ``__all__`` lists the package-level public symbols
only; sub-packages (``data``, ``models``) and modules (``cli``, ``train``,
``eval``, ``codec``, ``confidence``, ``tune``, ``op_allowlist``, ...) are
imported lazily via their dotted paths — see ADR-0911 for the package-
surface convention.
"""

__version__ = "0.2.0"

__all__ = ["__version__"]
