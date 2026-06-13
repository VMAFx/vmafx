# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Shared Python utilities for ``scripts/``.

Currently exposes:

* :mod:`scripts.lib.backlog_tracker` — typed wrapper for
  ``.workingdir2/BACKLOG.md`` and ``gh`` PR queries (Symphony-inspired
  tracker abstraction; see ADR-0355).

Per ADR-0911 the package surface is a namespace — sub-modules are
imported via their dotted paths.
"""

__all__ = [
    "backlog_tracker",
]
