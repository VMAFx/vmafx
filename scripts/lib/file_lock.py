# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Native nonblocking file locks; callers own the wait and retry policy."""

from __future__ import annotations

import errno
import sys
from typing import IO


def lock_file(handle: IO[bytes], *, acquire: bool) -> None:
    """Acquire/release a lock, with BlockingIOError for acquisition contention."""
    if sys.platform == "win32":
        import msvcrt  # noqa: PLC0415 - platform-specific

        handle.seek(0)
        mode = msvcrt.LK_NBLCK if acquire else msvcrt.LK_UNLCK
        try:
            msvcrt.locking(handle.fileno(), mode, 1)
        except OSError as exc:
            if acquire and exc.errno == errno.EACCES:
                raise BlockingIOError(exc.errno, exc.strerror) from exc
            raise
    else:
        import fcntl  # noqa: PLC0415 - platform-specific

        fcntl.flock(handle.fileno(), (fcntl.LOCK_EX | fcntl.LOCK_NB) if acquire else fcntl.LOCK_UN)
