#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Route the changed C/C++ files of a changed-files clang-tidy job (Q-341, Q-342).

Reads the changed paths, one per line, on stdin and prints on stdout the paths
the job lints with its compile database. A path it does not lint is named on
stderr with the reason.

``changed`` (the Tidy Changed job, CPU build): a changed file with a compile
command in the database is linted. A changed non-header without one is
deferred to the clang-tidy lane whose ``measured_sources``
(``scripts/ci/tidy-baseline-<lane>.json``) lists it, and that lane is named; if
no lane lists it, the job fails and names the file. A changed header is
linted on its own, as before, unless none of the translation units that
include it (directly or through other headers) has a command in the
database: it is then deferred, the same way, to a lane that measures one of
those units, or fails. A file (or every unit including a header) that no
lane can read and that holds a live entry of the declared exception list
``.config/lint-exceptions.d/clang-tidy-coverage.toml`` (ADR-1762; reason and
expiry, ``scripts/ci/lint_exceptions.py``) is skipped naming the entry; an
expired entry fails like a missing one.

``sycl`` (the Tidy SYCL job): a changed header that no C++ source includes
(a C-only header such as ``core/src/sycl/vmafx_sycl_internal.h``) is not
parsed on its own as C++; the C translation units of the database that
include it are linted instead. Every other path is passed through.

Exit 0 routed, 1 a file no lane measures (``changed``) or a C-only header
without an includer in the database (``sycl``), 2 unreadable input.
"""

from __future__ import annotations

import argparse
import json
import posixpath
import re
import sys
from collections import deque
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import lint_exceptions  # noqa: E402 -- the shared tracked-files reader

HEADER_SUFFIXES = (".h", ".hh", ".hpp", ".hxx", ".cuh", ".inc")
SOURCE_SUFFIXES = (".c", ".cc", ".cpp", ".cxx", ".cu", ".hip", ".mm")
CXX_SUFFIXES = (".cc", ".cpp", ".cxx", ".cu", ".hip", ".mm", ".hpp", ".hh", ".hxx", ".cuh")
# Directories a quoted include is resolved against after the including file's own.
INCLUDE_ROOTS = ("core/src", "core/include", "core/test", "core/src/feature", "core/tools")
INCLUDE = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.MULTILINE)
MAX_GRAPH_NODES = 1 << 16  # bound on the includer walk (HISS-02)


def measured_by_lane(root: Path) -> dict[str, list[str]]:
    """Each measured path and the lanes whose baseline lists it."""
    lanes: dict[str, list[str]] = {}
    for path in sorted((root / "scripts/ci").glob("tidy-baseline-*.json")):
        lane = path.stem.removeprefix("tidy-baseline-")
        for source in json.loads(path.read_text(encoding="utf-8")).get("measured_sources", []):
            lanes.setdefault(source, []).append(lane)
    return lanes


def database_paths(root: Path, database: Path) -> set[str]:
    """Repository-relative paths of the files the compile database has a command for."""
    found: set[str] = set()
    for entry in json.loads(database.read_text(encoding="utf-8")):
        path = (Path(entry["directory"]) / entry["file"]).resolve()
        if path.is_relative_to(root.resolve()):
            found.add(path.relative_to(root.resolve()).as_posix())
    return found


def resolve(including: str, name: str, tracked: frozenset[str]) -> str | None:
    """The tracked path a quoted include in ``including`` names, if any."""
    for base in (posixpath.dirname(including), *INCLUDE_ROOTS):
        candidate = posixpath.normpath(posixpath.join(base, name))
        if candidate in tracked:
            return candidate
    return None


def includers(root: Path, tracked: frozenset[str]) -> dict[str, set[str]]:
    """For each tracked header, the tracked files that include it directly."""
    graph: dict[str, set[str]] = {}
    for path in sorted(p for p in tracked if p.endswith(HEADER_SUFFIXES + SOURCE_SUFFIXES)):
        try:
            text = (root / path).read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for name in INCLUDE.findall(text):
            target = resolve(path, name, tracked)
            if target is not None and target != path:
                graph.setdefault(target, set()).add(path)
    return graph


def including_units(header: str, graph: dict[str, set[str]]) -> set[str]:
    """The translation units that include ``header``, directly or through headers."""
    units: set[str] = set()
    seen = {header}
    queue = deque([header])
    for _ in range(MAX_GRAPH_NODES):
        if not queue:
            break
        for parent in graph.get(queue.popleft(), set()):
            if parent in seen:
                continue
            seen.add(parent)
            if parent.endswith(SOURCE_SUFFIXES):
                units.add(parent)
            else:
                queue.append(parent)
    return units


COVERAGE_RULE = "clang-tidy-coverage"


def live_coverage_exceptions(root: Path) -> dict[str, lint_exceptions.Entry]:
    """The unexpired entries of the clang-tidy coverage exception list, by path."""
    entries, malformed = lint_exceptions.load(root)
    if malformed:
        raise ValueError("; ".join(malformed))
    now = lint_exceptions.today()
    return {e.path: e for e in entries if e.rule == COVERAGE_RULE and e.expires >= now}


def defer(
    path: str,
    units: set[str],
    lanes: dict[str, list[str]],
    excepted: dict[str, lint_exceptions.Entry],
) -> tuple[bool, str]:
    """Whether a lane measures ``units`` (or all are excepted), and the line that says so."""
    named = sorted({lane for unit in units for lane in lanes.get(unit, [])})
    if named:
        return True, f"tidy-changed: skip {path}: measured by the {', '.join(named)} lane(s)"
    if units and all(unit in excepted for unit in units):
        until = min(excepted[unit].expires for unit in units)
        return True, (
            f"tidy-changed: skip {path}: no lane reads it; declared {COVERAGE_RULE} exception "
            f"until {until} (.config/lint-exceptions.d/{COVERAGE_RULE}.toml)"
        )
    return False, (
        f"tidy-changed: {path} has no compile command in this build and no clang-tidy lane "
        "measures it (measured_sources of scripts/ci/tidy-baseline-*.json)"
    )


def route_changed(
    paths: list[str], commands: set[str], root: Path
) -> tuple[list[str], list[str], bool]:
    """Lintable paths, report lines, and whether every path was routed."""
    lanes = measured_by_lane(root)
    excepted = live_coverage_exceptions(root)
    graph: dict[str, set[str]] | None = None
    lint: list[str] = []
    report: list[str] = []
    ok = True
    for path in paths:
        if path in commands:
            lint.append(path)
            continue
        units = {path}
        if path.endswith(HEADER_SUFFIXES):
            graph = (
                graph if graph is not None else includers(root, lint_exceptions.tracked_files(root))
            )
            units = including_units(path, graph)
            if not units or units & commands:
                lint.append(path)
                continue
        deferred, line = defer(path, units, lanes, excepted)
        report.append(line)
        ok = ok and deferred
    return lint, report, ok


def route_sycl(
    paths: list[str], commands: set[str], root: Path
) -> tuple[list[str], list[str], bool]:
    """Pass paths through, replacing each C-only header by its includers in the database."""
    graph = includers(root, lint_exceptions.tracked_files(root))
    lint: list[str] = []
    report: list[str] = []
    ok = True
    for path in paths:
        units = including_units(path, graph) if path.endswith(HEADER_SUFFIXES) else set()
        if not units or any(u.endswith(CXX_SUFFIXES) for u in units):
            lint.append(path)
            continue
        through = sorted(units & commands)
        if through:
            report.append(f"tidy-sycl: {path} is C-only: linted through {', '.join(through)}")
            lint.extend(through)
        else:
            report.append(f"tidy-sycl: {path} is C-only and no including unit has a command here")
            ok = False
    return lint, report, ok


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("mode", choices=("changed", "sycl"))
    parser.add_argument("--db", type=Path, required=True, help="compile_commands.json")
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args(argv)
    paths = [line.strip() for line in sys.stdin.read().splitlines() if line.strip()]
    try:
        commands = database_paths(args.root, args.db)
        route = route_changed if args.mode == "changed" else route_sycl
        lint, report, ok = route(paths, commands, args.root)
    except (OSError, ValueError, KeyError, SystemExit) as exc:
        print(f"tidy_changed_route: {exc}", file=sys.stderr)
        return 2
    for line in report:
        print(line, file=sys.stderr)
    for path in dict.fromkeys(lint):  # each path once, in first-seen order
        print(path)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
