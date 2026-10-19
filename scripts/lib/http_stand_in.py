# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""A loopback HTTP server that answers what a test plans, for download tests.

    with StandIn([504, 504], body=b"payload") as server:
        fetch(server.url("yuv/a.yuv"))      # 504, 504, then 200 with the body
        assert server.requests == ["/yuv/a.yuv"] * 3

Every GET takes the next planned status; with the plan used up it answers 200.
A 200 carries `bodies[path]` when the path has an entry, else `body`. Standard
library only; the server listens on 127.0.0.1 and an ephemeral port.
"""

from __future__ import annotations

import http.server
import threading
from collections.abc import Iterable, Mapping
from types import TracebackType

ERROR_BODY = b"stand-in error\n"
OK = 200


class StandIn:
    """Serve planned statuses in order; record the path of every request."""

    def __init__(
        self,
        statuses: Iterable[int] = (),
        body: bytes = b"",
        bodies: Mapping[str, bytes] | None = None,
    ) -> None:
        self.statuses = list(statuses)
        self.body = body
        self.bodies = dict(bodies or {})
        self.requests: list[str] = []
        self._server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), self._handler())
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)

    def _handler(self) -> type[http.server.BaseHTTPRequestHandler]:
        stand_in = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self) -> None:  # the name http.server dispatches on
                stand_in.requests.append(self.path)
                status = stand_in.statuses.pop(0) if stand_in.statuses else OK
                payload = stand_in.bodies.get(self.path, stand_in.body)
                content = payload if status == OK else ERROR_BODY
                self.send_response(status)
                self.send_header("Content-Length", str(len(content)))
                self.end_headers()
                self.wfile.write(content)

            def log_message(self, format: str, *args: object) -> None:
                return

        return Handler

    @property
    def base(self) -> str:
        """`http://127.0.0.1:<port>`, without a trailing slash."""
        return f"http://127.0.0.1:{self._server.server_address[1]}"

    def url(self, path: str) -> str:
        return f"{self.base}/{path.lstrip('/')}"

    def __enter__(self) -> StandIn:
        self._thread.start()
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        traceback: TracebackType | None,
    ) -> None:
        self._server.shutdown()
        self._server.server_close()
        self._thread.join()
