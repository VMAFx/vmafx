#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""scripts/lib/http_stand_in.py answers the plan in order, then 200."""

from __future__ import annotations

import contextlib
import sys
import unittest
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.lib.http_stand_in import ERROR_BODY, StandIn


def get(url: str) -> tuple[int, bytes]:
    """(status, body) of one GET; an HTTP error is an answer, not a failure."""
    try:
        with urllib.request.urlopen(url, timeout=30) as response:  # noqa: S310 (loopback)
            return response.status, response.read()
    except urllib.error.HTTPError as error:
        with contextlib.closing(error):
            return error.code, error.read()


class StandInTests(unittest.TestCase):
    def test_the_plan_is_answered_in_order_and_then_200(self) -> None:
        with StandIn([504, 404], body=b"payload") as server:
            answers = [get(server.url("a")) for _ in range(4)]
            self.assertEqual(server.requests, ["/a"] * 4)
        self.assertEqual(
            answers,
            [(504, ERROR_BODY), (404, ERROR_BODY), (200, b"payload"), (200, b"payload")],
        )

    def test_a_path_can_have_a_body_of_its_own(self) -> None:
        with StandIn(body=b"default", bodies={"/yuv/b": b"other"}) as server:
            self.assertEqual(get(server.url("yuv/a")), (200, b"default"))
            self.assertEqual(get(server.url("/yuv/b")), (200, b"other"))
            self.assertEqual(server.requests, ["/yuv/a", "/yuv/b"])

    def test_the_server_is_gone_after_the_block(self) -> None:
        with StandIn() as server:
            url = server.url("a")
            self.assertTrue(url.startswith("http://127.0.0.1:"))
            self.assertEqual(get(url), (200, b""))
        with self.assertRaises(urllib.error.URLError):
            get(url)


if __name__ == "__main__":
    unittest.main()
