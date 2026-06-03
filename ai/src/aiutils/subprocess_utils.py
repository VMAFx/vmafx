# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Subprocess execution utilities."""

from __future__ import annotations

import subprocess
from typing import Any, cast


def run_cmd(
    cmd: list[str],
    *,
    capture: bool = False,
    check: bool = True,
    timeout: float | None = None,
    **kwargs: Any,
) -> subprocess.CompletedProcess[Any]:
    """Run a subprocess command with standard error handling.

    Args:
        cmd: Command and arguments to execute.
        capture: If True, capture stdout and stderr (text mode). Equivalent to
            ``capture_output=True, text=True``.
        check: If True (default), raise CalledProcessError on non-zero exit.
            Pass False to inspect returncode manually.
        timeout: Optional timeout in seconds. Raises TimeoutExpired on breach.
        **kwargs: Additional keyword arguments forwarded to subprocess.run().
            Typed as :class:`Any` because ``subprocess.run`` is an
            overload-heavy passthrough; we forward verbatim and let the
            ``subprocess`` stubs validate at the call boundary.

    Returns:
        CompletedProcess instance. When capture=True, stdout and stderr are
        available as strings on the returned object; otherwise as bytes —
        which is why the generic parameter is :class:`Any`.

    Raises:
        subprocess.CalledProcessError: If check=True and the command exits non-zero.
        subprocess.TimeoutExpired: If timeout is set and the command exceeds it.
        FileNotFoundError: If the executable is not found on PATH.
    """
    if capture:
        kwargs.setdefault("capture_output", True)
        kwargs.setdefault("text", True)
    # Cast through CompletedProcess[Any]: the runtime ``mode`` (text vs.
    # bytes) is decided by the ``text``/``capture`` kwargs the caller
    # passes, which the overload chain can't reconcile against a
    # ``**kwargs: Any`` forward.
    return cast(
        "subprocess.CompletedProcess[Any]",
        subprocess.run(cmd, check=check, timeout=timeout, **kwargs),
    )
