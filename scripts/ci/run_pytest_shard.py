#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Run one shard of a pytest selection, so no CI step runs the whole suite.

The Coverage Gate runs the instrumented Python suite (about 890 items, 33
minutes on the hosted runner, measured on master 777647fc0, job 117249828983).
praetor bounds a test step at 30 minutes (cordanaLLM/praetor#1381), so the
suite runs as N steps of one shard each::

    python3 scripts/ci/run_pytest_shard.py --shard 1/4 -- python/test/ -v ...

Everything after ``--`` is pytest's own command line. pytest runs in this
process with one more plugin: after every other plugin has filtered the
collection (``-m``, ``--deselect``, ``--ignore``, ``-k``), it keeps every N-th
item in collection order starting at item I and reports the rest as
deselected. Collection, rootdir and node ids stay pytest's own (python/tox.ini
makes python/ the rootdir, so node ids handed back on a command line from the
repository root would not resolve), a shard sees exactly what the unsharded
run saw, and a ``timeout`` around this script signals pytest directly.
Round-robin over the collection order spreads each file's slow tests over
every shard, which a split by file cannot do for quality_runner_test.py (14 of
the 33 minutes). Run every shard 1/N .. N/N and the union is the selection,
each item once.

Exit codes are pytest's: 5 when the shard holds no item, 2 for a collection
error; argparse exits 2 on a bad ``--shard``.
"""

from __future__ import annotations

import argparse
import sys
from collections.abc import Callable, Sequence
from typing import Any, TypeVar, cast

import pytest

_Hook = TypeVar("_Hook", bound=Callable[..., None])


def trylast(hook: _Hook) -> _Hook:
    """Mark a hook to run after every other plugin's, keeping its type for mypy."""
    return cast(_Hook, pytest.hookimpl(trylast=True)(hook))


def parse_shard(text: str) -> tuple[int, int]:
    """Return (index, count) of an ``I/N`` shard spelling, 1 <= I <= N."""
    index_text, sep, count_text = text.partition("/")
    if not sep or not index_text.isdigit() or not count_text.isdigit():
        raise argparse.ArgumentTypeError(f"shard must be I/N, got {text!r}")
    index, count = int(index_text), int(count_text)
    if count < 1 or not 1 <= index <= count:
        raise argparse.ArgumentTypeError(f"shard must satisfy 1 <= I <= N, got {text!r}")
    return index, count


def split_shard(items: Sequence[Any], index: int, count: int) -> tuple[list[Any], list[Any]]:
    """Return (kept, dropped): every count-th item from the index-th (1-based), and the rest."""
    kept: list[Any] = []
    dropped: list[Any] = []
    for position, item in enumerate(items):
        (kept if position % count == index - 1 else dropped).append(item)
    return kept, dropped


class ShardSelector:
    """pytest plugin that keeps one round-robin shard of the filtered collection."""

    def __init__(self, index: int, count: int) -> None:
        self.index = index
        self.count = count

    # trylast: run after the mark and keyword plugins, so the shard splits what they kept.
    @trylast
    def pytest_collection_modifyitems(self, config: pytest.Config, items: list[Any]) -> None:
        kept, dropped = split_shard(items, self.index, self.count)
        if dropped:
            config.hook.pytest_deselected(items=dropped)
        items[:] = kept
        print(f"run_pytest_shard: shard {self.index}/{self.count}: {len(kept)} items kept")


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=(__doc__ or "").splitlines()[0])
    parser.add_argument("--shard", type=parse_shard, required=True, metavar="I/N")
    parser.add_argument("pytest_args", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    pytest_args = args.pytest_args
    if pytest_args[:1] == ["--"]:
        pytest_args = pytest_args[1:]
    index, count = args.shard
    return int(pytest.main(pytest_args, plugins=[ShardSelector(index, count)]))


if __name__ == "__main__":
    sys.exit(main())
