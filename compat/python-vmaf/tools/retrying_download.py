# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
"""Download one file, retrying a transient failure for minutes.

The one retry policy of the test-fixture downloads: ``download_reactively()``
(``vmaf/config.py``) calls :func:`retrieve`, and
``scripts/test/fetch-test-yuvs.sh`` runs this file as a script::

    python3 -I compat/python-vmaf/tools/retrying_download.py URL PATH

Standard library only, so the script form needs neither the ``vmaf`` package
nor its dependencies.

A transient failure is one a later attempt can cure: the HTTP statuses of
``TRANSIENT_HTTP_STATUS``, a refused, reset or timed-out connection, a failed
name lookup, a short body. A 404, a certificate that does not verify or a full
disk fails at once.
"""

from __future__ import annotations

import contextlib
import http.client
import os
import ssl
import sys
import time
import urllib.error
import urllib.request
from collections.abc import Callable

# Waits between attempts: seven attempts, 275 s of waiting. The outage this
# answers lasted at least three minutes (the fixture host answered HTTP 504 at
# 14:15 and at 14:18 UTC on 2026-10-10). The sum stays below the 600 s
# per-test timeout of the Coverage Gate's pytest run, whose tests download
# their fixtures through download_reactively().
RETRY_DELAYS_S: tuple[float, ...] = (5, 10, 20, 40, 80, 120)
TRANSIENT_HTTP_STATUS = frozenset({408, 425, 429, 500, 502, 503, 504, 522, 524})
USAGE = "usage: retrying_download.py URL PATH"


def transient(error: BaseException) -> bool:
    """Whether a later attempt can succeed where this one failed."""
    if isinstance(error, urllib.error.HTTPError):
        return error.code in TRANSIENT_HTTP_STATUS
    if isinstance(error, urllib.error.URLError):
        return not isinstance(error.reason, ssl.SSLCertVerificationError)
    return isinstance(error, (TimeoutError, ConnectionError, http.client.HTTPException))


def retrieve(
    url: str,
    path: str,
    delays: tuple[float, ...] = RETRY_DELAYS_S,
    sleep: Callable[[float], None] | None = None,
) -> int:
    """Download ``url`` to ``path``; return the number of attempts it took.

    Up to ``len(delays) + 1`` attempts, ``delays[i]`` seconds after the failed
    attempt ``i + 1`` (``sleep`` waits; ``time.sleep`` when not given). The
    last error, or the first one that is not transient, is raised unchanged.
    """
    attempts = len(delays) + 1
    wait = time.sleep if sleep is None else sleep
    for attempt, delay in enumerate((*delays, None), start=1):
        try:
            # The URL is the caller's fixture address (http or https, checked
            # by main() for the script form).
            urllib.request.urlretrieve(url, path)  # noqa: S310
        except Exception as error:
            if delay is None or not transient(error):
                raise
            if isinstance(error, urllib.error.HTTPError):
                # An HTTPError holds its response open until it is closed.
                error.close()
            print(
                f"download of {url}: attempt {attempt} of {attempts} failed ({error}), "
                f"next in {delay:g} s",
                file=sys.stderr,
                flush=True,
            )
            wait(delay)
        else:
            return attempt
    raise AssertionError("unreachable: the last attempt returns or raises")


def main(argv: list[str] | None = None) -> int:
    """Download URL to PATH through a sibling ``.part`` file; 0, 1 or 2 (usage)."""
    arguments = sys.argv[1:] if argv is None else argv
    if len(arguments) != 2 or not arguments[0].startswith(("https://", "http://")):
        print(USAGE, file=sys.stderr)
        return 2
    url, destination = arguments
    partial = destination + ".part"
    try:
        attempts = retrieve(url, partial)
        os.replace(partial, destination)
    except Exception as error:  # noqa: BLE001 (every failure is reported and exits 1)
        with contextlib.suppress(OSError):
            os.remove(partial)
        if isinstance(error, urllib.error.HTTPError):
            error.close()
        print(f"retrying_download: {url}: {error}", file=sys.stderr)
        return 1
    if attempts > 1:
        print(f"retrying_download: {url}: done on attempt {attempts}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
