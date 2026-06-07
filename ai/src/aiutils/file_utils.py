# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""File I/O utilities."""

from __future__ import annotations

import hashlib
import os
import tempfile
from pathlib import Path


def write_text_atomic(path: Path, text: str, *, encoding: str = "utf-8") -> None:
    """Write *text* to *path* atomically using a same-directory temp file.

    Writes to a temporary file in ``path.parent``, then renames it to
    ``path`` via :func:`os.replace`.  This guarantees that a reader never
    sees a partially-written file: the destination either contains the
    previous content or the new content, never a truncated intermediate.

    Args:
        path:     Destination file path.  The parent directory must exist.
        text:     Text content to write.
        encoding: Character encoding passed to :func:`open`.  Defaults to
                  ``"utf-8"``.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp_str = tempfile.mkstemp(dir=path.parent)
    tmp = Path(tmp_str)
    try:
        with os.fdopen(fd, "w", encoding=encoding) as fh:
            fh.write(text)
        os.replace(tmp_str, path)
    except Exception:
        tmp.unlink(missing_ok=True)
        raise


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
