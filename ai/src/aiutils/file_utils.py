# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""File I/O utilities."""

from __future__ import annotations

import hashlib
from pathlib import Path


def sha256(path: Path) -> str:
    """Compute SHA-256 hexdigest of a file using streaming 1 MiB chunks.

    Args:
        path: File path to hash.

    Returns:
        Lowercase hex string of the file's SHA-256 digest.
    """
    h = hashlib.sha256()
    with path.open("rb") as fh:
        while True:
            chunk = fh.read(1 << 20)  # 1 MiB
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()
