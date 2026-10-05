# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Dependency order without recursion (HISS-01): Kahn's algorithm, bounded loops."""

from __future__ import annotations

from .model import DefinitionError


def order(nodes: list[str], depends: dict[str, set[str]], what: str) -> list[str]:
    """`nodes` sorted so every node follows what it depends on; ties keep input order.

    `depends[n]` lists the nodes `n` needs first; names outside `nodes` are
    ignored. A cycle stops generation and names its members.
    """
    known = set(nodes)
    pending = {n: {d for d in depends.get(n, set()) if d in known and d != n} for n in nodes}
    loops = [n for n in nodes if n in depends.get(n, set())]
    if loops:
        raise DefinitionError(f"{what} cycle: {loops[0]} depends on itself")
    result: list[str] = []
    for _ in range(len(nodes)):
        ready = next((n for n in nodes if n in pending and not pending[n]), None)
        if ready is None:
            raise DefinitionError(f"{what} cycle among {sorted(pending)}")
        result.append(ready)
        del pending[ready]
        for needs in pending.values():
            needs.discard(ready)
    return result
