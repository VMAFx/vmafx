#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Queue provenance decisions and workflow output."""

import tempfile
from pathlib import Path

from scripts.ci import merge_group_provenance as provenance


def response(entries, more=False):
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


def test_queue_prefix():
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


def test_errors_fail_closed():
    for value in (
        {"errors": [{"message": "denied"}]},
        {"data": {"repository": {"mergeQueue": None}}},
        {"data": {}},
        response([(1, "ours", False)]),
    ):
        if "entries" in str(value):
            del value["data"]["repository"]["mergeQueue"]["entries"]["nodes"][0]["pullRequest"]
        assert not provenance.decide(value, "ours")[0]


def test_network_error_and_output(capsys):
    def fail(*args):
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
