# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""VMAFx Python binding (ctypes, standard library only; ADR-1852).

Everything is generated into `_api` from core/api/vmafx.toml. Load the library
explicitly: `Library(path)` or `$VMAFX_LIBRARY`; there is no search fallback.
"""

from ._api import (
    ABI_VERSION,
    Backend,
    Context,
    ContextConfig,
    Library,
    LogLevel,
    Status,
    VmafxError,
    VmafxPending,
    load,
)

__all__ = [
    "ABI_VERSION",
    "Backend",
    "Context",
    "ContextConfig",
    "Library",
    "LogLevel",
    "Status",
    "VmafxError",
    "VmafxPending",
    "load",
]
