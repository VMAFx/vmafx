#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Queue provenance decisions and workflow output."""

import tempfile
from pathlib import Path
from typing import Any, NoReturn

import pytest

from scripts.ci import merge_group_provenance as provenance


def response(entries: list[tuple[int, str, bool]], more: bool = False) -> dict[str, Any]:
    nodes = [
        {
            "position": position,
            "headCommit": {"oid": oid},
            "pullRequest": {"number": position, "isCrossRepository": cross},
        }
        for position, oid, cross in entries
    ]
    return {
        "data": {
            "repository": {
                "mergeQueue": {"entries": {"pageInfo": {"hasNextPage": more}, "nodes": nodes}}
            }
        }
    }


def test_queue_prefix() -> None:
    for entries, expected in [
        ([(1, "a", False), (2, "ours", False)], True),
        ([(1, "a", True), (2, "ours", False)], False),
        ([(1, "ours", False), (2, "b", True)], True),
        ([(1, "ours", False)], True),
        ([(1, "a", False)], False),
        ([], False),
    ]:
        assert provenance.decide(response(entries), "ours")[0] is expected
    assert provenance.decide(response([], True), "ours") == (
        False,
        "entry not found; more pages available",
    )


def test_invalid_entry_types() -> None:
    for field, value in [
        ("position", True),
        ("position", 0),
        ("position", "1"),
        ("number", True),
        ("isCrossRepository", 1),
        ("isCrossRepository", "false"),
    ]:
        r = response([(1, "ours", False)])
        node = r["data"]["repository"]["mergeQueue"]["entries"]["nodes"][0]
        target = node if field == "position" else node["pullRequest"]
        target[field] = value
        assert provenance.decide(r, "ours") == (False, "missing or invalid response field")


def test_errors_fail_closed() -> None:
    missing_pull_request = {
        "data": {
            "repository": {
                "mergeQueue": {
                    "entries": {
                        "pageInfo": {"hasNextPage": False},
                        "nodes": [{"position": 1, "headCommit": {"oid": "ours"}}],
                    }
                }
            }
        }
    }
    value: dict[str, Any]
    for value in (
        {"errors": [{"message": "denied"}]},
        {"data": {"repository": {"mergeQueue": None}}},
        {"data": {}},
        missing_pull_request,
    ):
        assert not provenance.decide(value, "ours")[0]


def test_network_error_and_output(capsys: pytest.CaptureFixture[str]) -> None:
    def fail(*args: object) -> NoReturn:
        raise OSError("offline")

    result = provenance.run(
        {"GITHUB_REPOSITORY": "a/b", "GITHUB_SHA": "ours", "GH_TOKEN": "x" * 3}, fail
    )
    assert result == (False, "queue lookup error")
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "output"
        provenance.write_output(str(path), result)
        assert path.read_text() == "same_repo=false\nreason=queue lookup error\n"
        provenance.write_output(str(path), (True, "ok"))
        assert path.read_text().endswith("same_repo=true\nreason=ok\n")
    assert capsys.readouterr().out == "::error::queue lookup error\n::notice::ok\n"
