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
import urllib.parse
import urllib.request
from collections.abc import Callable

# Waits between attempts: seven attempts, 275 s of waiting. The outage this
# answers lasted at least three minutes (the fixture host answered HTTP 504 at
# 14:15 and at 14:18 UTC on 2026-10-10). The sum stays below the 600 s
# per-test timeout of the Coverage Gate's pytest run, whose tests download
# their fixtures through download_reactively().
RETRY_DELAYS_S: tuple[float, ...] = (5, 10, 20, 40, 80, 120)
TRANSIENT_HTTP_STATUS = frozenset({408, 425, 429, 500, 502, 503, 504, 522, 524})
# One transfer is bounded: a socket read waits at most READ_TIMEOUT_S, and the
# whole body must arrive within TRANSFER_TIMEOUT_S. CI tool downloads used
# curl --max-time 300 before #3349; the fixture downloads keep the same bound.
READ_TIMEOUT_S = 60
TRANSFER_TIMEOUT_S = 300
READ_CHUNK = 1 << 20
USAGE = "usage: retrying_download.py URL PATH"


def transient(error: BaseException) -> bool:
    """Whether a later attempt can succeed where this one failed."""
    if isinstance(error, urllib.error.HTTPError):
        return error.code in TRANSIENT_HTTP_STATUS
    if isinstance(error, urllib.error.URLError):
        return not isinstance(error.reason, ssl.SSLCertVerificationError)
    return isinstance(error, (TimeoutError, ConnectionError, http.client.HTTPException))


def _set_read_timeout(response: object, seconds: float) -> None:
    """Cap the next blocking socket read at SECONDS, when the socket is reachable."""
    sock = getattr(getattr(getattr(response, "fp", None), "raw", None), "_sock", None)
    if sock is not None:
        sock.settimeout(max(0.001, seconds))


def _copy_body(response: object, path: str, deadline: float) -> None:
    """Stream a response to PATH, failing on a total deadline or a short body."""
    expected = response.headers.get("Content-Length")  # type: ignore[attr-defined]
    received = 0
    with open(path, "wb") as output:
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"transfer exceeded {TRANSFER_TIMEOUT_S} s")
            _set_read_timeout(response, min(READ_TIMEOUT_S, remaining))
            # read1 performs at most one socket read, so a trickling server
            # cannot hold the loop for a whole chunk between deadline checks.
            chunk = response.read1(READ_CHUNK)  # type: ignore[attr-defined]
            if not chunk:
                break
            output.write(chunk)
            received += len(chunk)
    if expected is not None:
        try:
            declared = int(expected)
        except ValueError:
            declared = -1
        if declared >= 0 and received != declared:
            raise http.client.IncompleteRead(b"", max(0, declared - received))


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

    Each transfer is bounded: a read waits at most ``READ_TIMEOUT_S`` and the
    whole body must arrive within ``TRANSFER_TIMEOUT_S``. A stalled connection
    therefore raises and joins the retry loop instead of hanging the job.
    """
    attempts = len(delays) + 1
    wait = time.sleep if sleep is None else sleep
    parsed = urllib.parse.urlparse(url)
    if parsed.scheme not in {"http", "https"} or not parsed.netloc:
        raise ValueError(f"retrying_download: only http(s) URLs are supported: {url}")
    open_url = urllib.request.urlopen  # nosec B310 - the scheme is checked above
    for attempt, delay in enumerate((*delays, None), start=1):
        try:
            # The URL is the caller's fixture address, checked above for this
            # and the script form.
            with open_url(url, timeout=READ_TIMEOUT_S) as response:
                _copy_body(response, path, time.monotonic() + TRANSFER_TIMEOUT_S)
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
