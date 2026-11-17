#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Fail closed when a merge group contains a cross-repository queue entry."""

import os
import sys
from collections.abc import Callable
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.ci import ci_tier

QUERY = """query($owner: String!, $name: String!) {
  repository(owner: $owner, name: $name) {
    mergeQueue(branch: "master") {
      entries(first: 100) {
        pageInfo { hasNextPage }
        nodes { position headCommit { oid } pullRequest { number isCrossRepository } }
      }
    }
  }
}"""


def decide(response: Any, sha: str) -> tuple[bool, str]:
    """Evaluate the queue prefix ending at the matching group head."""
    try:
        if response.get("errors"):
            return False, "GraphQL error"
        queue = response["data"]["repository"]["mergeQueue"]
        if queue is None:
            return False, "mergeQueue is null"
        entries = queue["entries"]
        more = entries["pageInfo"]["hasNextPage"]
        nodes = entries["nodes"]
        if not isinstance(more, bool) or not isinstance(nodes, list) or not sha:
            raise ValueError("invalid entries")
        positions = []
        for node in nodes:
            position = node["position"]
            oid = node["headCommit"]["oid"]
            pr = node["pullRequest"]
            cross = pr["isCrossRepository"]
            if (
                type(position) is not int
                or position < 1
                or not isinstance(oid, str)
                or not oid
                or type(pr["number"]) is not int
                or type(cross) is not bool
            ):
                raise ValueError("invalid entry")
            positions.append((position, oid, cross))
        matches = [position for position, oid, _ in positions if oid == sha]
        if len(matches) != 1:
            return False, "entry not found; more pages available" if more else "entry not found"
        if len({position for position, _, _ in positions}) != len(positions):
            raise ValueError("duplicate position")
        if any(cross for position, _, cross in positions if position <= matches[0]):
            return False, "cross-repository entry in queue prefix"
        return True, "queue prefix contains only same-repository pull requests"
    except AttributeError, KeyError, TypeError, ValueError:
        return False, "missing or invalid response field"


def fetch(owner: str, name: str, token: str) -> Any:
    """Fetch queue data using the workflow token."""
    return ci_tier.api_request(
        "POST",
        "https://api.github.com/graphql",
        token,
        {"query": QUERY, "variables": {"owner": owner, "name": name}},
    )


def run(environ: Any, fetcher: Callable[..., Any] = fetch) -> tuple[bool, str]:
    """Obtain and evaluate queue data; errors deny access."""
    try:
        owner, name = environ["GITHUB_REPOSITORY"].split("/")
        sha, token = environ["GITHUB_SHA"], environ["GH_TOKEN"]
        if not owner or not name or not sha or not token:
            raise ValueError("empty input")
        return decide(fetcher(owner, name, token), sha)
    except Exception:  # Network, JSON, HTTP and configuration errors all fail closed.
        return False, "queue lookup error"


def write_output(path: str, result: tuple[bool, str]) -> None:
    """Write a single-line reason and the workflow output."""
    allowed, reason = result
    reason = reason.replace("\n", " ").replace("\r", " ")
    with Path(path).open("a", encoding="utf-8") as output:
        output.write(f"same_repo={str(allowed).lower()}\nreason={reason}\n")
    print(f"::{'notice' if allowed else 'error'}::{reason}")


if __name__ == "__main__":
    write_output(os.environ["GITHUB_OUTPUT"], run(os.environ))
