#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Prove that splitting one ``AGENTS.md`` into ``AGENTS.d/`` pages lost nothing.

Usage:
    agents_migration_check.py --old-ref origin/master scripts/ci
    agents_migration_check.py --old-file /path/to/old/AGENTS.md scripts/ci

The old file is the single ``<directory>/AGENTS.md`` from before the split;
the new text is ``<directory>/AGENTS.d/_index.md`` plus every page next to it.
Four checks, all must hold (exit 1 otherwise):

* units: every paragraph, top-level list item, table row and code block of the
  old file is in the new text exactly as often as before, byte for byte. Only
  the targets of relative links differ, because a page sits one directory
  deeper; both sides are compared after resolving them to the repository
  root. No unit dropped, none duplicated, none added;
* headings: every heading text of the old file survives (its level and the
  number of pages that repeat it are free);
* tokens: every load-bearing token of the old file (code spans, paths, ADR /
  PR / issue / ledger ids, flags, identifiers, numbers, link targets) is in
  the new text;
* layout: no table row without a header above it, and the generated index is
  fresh.

Pages add only structure: front matter, a title, repeated headings and table
headers (ADR-1454).
"""

from __future__ import annotations

import argparse
import posixpath
import re
import statistics
import sys
from collections import Counter
from collections.abc import Sequence
from dataclasses import dataclass, field, replace
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from scripts.docs import agents_index  # noqa: E402
from scripts.lib import git_env  # noqa: E402
from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

HEADING = "heading"
TABLE_HEAD = "table-head"
COMMENT = "comment"
TABLE_ROW = "table-row"
ITEM = "item"
TEXT = "text"

MAX_REPORTED = 40
SNIPPET_CHARS = 96

HEADING_RE = re.compile(r"(#{1,6})\s+(.*\S)\s*")
ITEM_RE = re.compile(r"(?:[-*+]|\d{1,9}[.)])\s")
DELIMITER_RE = re.compile(r"\|\s*:?-+:?\s*(?:\|\s*:?-+:?\s*)*\|?\s*")
ANCHOR_LINK_RE = re.compile(r"\]\(#[^)\s]*\)")

TOKEN_PATTERNS = (
    r"`[^`\n]+`",  # code spans
    r"\bADR-\d{3,5}\b",
    r"\bResearch-\d{3,5}\b",
    r"\bHISS-\d{2}\b",
    r"\bT-[A-Z0-9]+(?:-[A-Z0-9]+)+\b",  # ledger row ids
    r"#\d{2,5}\b",  # PR / issue numbers
    r"\b[0-9a-f]{7,40}\b",  # commit hashes
    r"(?<![\w/])--?[a-zA-Z][\w-]{2,}",  # flags
    r"\b[\w.-]+/[\w./-]+\b",  # paths
    r"\b[A-Za-z]\w*(?:_\w+)+\b",  # snake_case identifiers
    r"\b[\w-]+\.(?:c|h|cpp|hpp|cu|hip|mm|metal|py|go|rs|sh|md|json|yaml|yml|toml|txt|build)\b",
    r"\b\d+(?:\.\d+)?(?:e-?\d+)?\b",  # numbers
    r"\]\([^)\s]+\)",  # link targets
)
TOKEN_RE = re.compile("|".join(f"(?:{pattern})" for pattern in TOKEN_PATTERNS))


@dataclass(frozen=True)
class Unit:
    """One block of a Markdown file: where it starts and its verbatim text."""

    kind: str
    text: str
    source: str
    line: int


@dataclass
class _Splitter:
    source: str
    units: list[Unit] = field(default_factory=list)
    lines: list[str] = field(default_factory=list)
    kind: str = ""
    start: int = 0
    fence: str = ""
    blanks: int = 0

    def flush(self) -> None:
        if self.lines:
            self.units.append(Unit(self.kind, "\n".join(self.lines), self.source, self.start))
        self.lines, self.kind, self.blanks = [], "", 0

    def single(self, kind: str, number: int, line: str) -> None:
        self.flush()
        self.units.append(Unit(kind, line, self.source, number))

    def append(self, kind: str, number: int, line: str) -> None:
        if not self.lines:
            self.kind, self.start = kind, number
        self.lines += [""] * self.blanks + [line]
        self.blanks = 0

    def feed(self, number: int, line: str) -> None:
        opener = agents_index.FENCE_RE.match(line)
        if self.fence:
            self.lines.append(line)
            if opener and opener.group(1).startswith(self.fence):
                self.fence = ""
            return
        if not line.strip():
            self.blanks += 1
            if self.kind != ITEM:
                self.flush()
            return
        if self.blanks and not line.startswith("  "):
            self.flush()
        self._feed_text(number, line, opener)

    def _feed_text(self, number: int, line: str, opener: re.Match[str] | None) -> None:
        if opener:
            self.append(TEXT, number, line)
            self.fence = opener.group(1)[:3]
        elif HEADING_RE.fullmatch(line):
            self.single(HEADING, number, line)
        elif agents_index.COMMENT_RE.fullmatch(line):
            self.single(COMMENT, number, line)
        elif line.startswith("|"):
            self.single(TABLE_ROW, number, line)
        elif ITEM_RE.match(line):
            self.flush()
            self.append(ITEM, number, line)
        else:
            self.append(TEXT, number, line)


def _is_table_head(units: Sequence[Unit], position: int) -> bool:
    unit = units[position]
    if DELIMITER_RE.fullmatch(unit.text):
        return True
    following = units[position + 1] if position + 1 < len(units) else None
    return (
        following is not None
        and following.kind == TABLE_ROW
        and following.line == unit.line + 1
        and DELIMITER_RE.fullmatch(following.text) is not None
    )


def split_units(text: str, source: str) -> list[Unit]:
    """Split Markdown ``text`` into headings, table headers, comments and content."""

    splitter = _Splitter(source)
    for number, line in enumerate(text.splitlines(), start=1):
        splitter.feed(number, line.rstrip())
    splitter.flush()
    units: list[Unit] = []
    for position, unit in enumerate(splitter.units):
        kind = unit.kind
        if kind == TABLE_ROW:
            kind = TABLE_HEAD if _is_table_head(splitter.units, position) else TABLE_ROW
        units.append(replace(unit, kind=kind))
    return units


def headerless_rows(units: Sequence[Unit]) -> list[Unit]:
    """Return the table rows that start a table without a header row."""

    found: list[Unit] = []
    previous: Unit | None = None
    for unit in units:
        adjacent = (
            previous is not None
            and previous.kind in (TABLE_HEAD, TABLE_ROW)
            and previous.source == unit.source
            and previous.line + 1 == unit.line
        )
        if unit.kind == TABLE_ROW and not adjacent:
            found.append(unit)
        previous = unit
    return found


def canonical_target(target: str, base: str) -> str:
    """Resolve a relative link ``target`` written in ``base`` to the repository root."""

    path, sep, fragment = target.partition("#")
    return "/" + posixpath.normpath(posixpath.join(base, path)) + sep + fragment


def canonical_text(text: str, base: str) -> str:
    """Return ``text`` with every relative link target resolved to the root."""

    return agents_index.map_links(text, lambda target: canonical_target(target, base))


def heading_text(unit: Unit) -> str:
    """Return the text of a heading unit without its ``#`` markers."""

    match = HEADING_RE.fullmatch(unit.text)
    return match.group(2) if match else unit.text


def tokens(text: str) -> set[str]:
    """Return every load-bearing token of ``text``."""

    return {match.group(0) for match in TOKEN_RE.finditer(text)}


def missing_tokens(old: str, new: str) -> list[str]:
    """Return the tokens of ``old`` that ``new`` no longer contains."""

    return sorted(
        token for token in tokens(old) if token not in new and token.strip("`") not in new
    )


@dataclass
class Report:
    """Everything the check measured; ``failures`` is empty when nothing was lost."""

    lines: list[str] = field(default_factory=list)
    failures: int = 0

    def finding(self, label: str, items: Sequence[str]) -> None:
        self.lines.append(f"  {label}: {len(items)}")
        self.failures += len(items)
        self.lines += [f"    {item}" for item in items[:MAX_REPORTED]]
        if len(items) > MAX_REPORTED:
            self.lines.append(f"    ... {len(items) - MAX_REPORTED} more")


def _snippet(unit: Unit) -> str:
    first = unit.text.splitlines()[0] if unit.text else ""
    return f"{unit.source}:{unit.line}: {first[:SNIPPET_CHARS]}"


def _content(units: Sequence[Unit], base: str) -> list[tuple[str, Unit]]:
    wanted = (TABLE_ROW, ITEM, TEXT)
    return [(canonical_text(unit.text, base), unit) for unit in units if unit.kind in wanted]


def compare_units(
    old: Sequence[tuple[str, Unit]], new: Sequence[tuple[str, Unit]], report: Report
) -> None:
    """Record every old unit that is missing and every new unit that is extra."""

    old_count = Counter(text for text, _ in old)
    new_count = Counter(text for text, _ in new)
    dropped = [_snippet(unit) for text, unit in old if new_count[text] < old_count[text]]
    extra = [(text, unit) for text, unit in new if new_count[text] > old_count[text]]
    report.finding("units dropped", sorted(set(dropped)))
    report.finding(
        "units duplicated", sorted({_snippet(unit) for text, unit in extra if old_count[text]})
    )
    report.finding(
        "units added", sorted({_snippet(unit) for text, unit in extra if not old_count[text]})
    )


def _sizes(directory: str, root: Path, report: Report) -> None:
    pages_dir = root / directory / agents_index.PAGES_DIR
    index = root / directory / agents_index.INDEX_NAME
    sizes = sorted(
        (path.stat().st_size, path.name)
        for path in pages_dir.glob("*.md")
        if path.name != agents_index.HEAD_NAME
    )
    values = [size for size, _ in sizes]
    report.lines.append(
        f"  new index: {index.stat().st_size if index.is_file() else 0} bytes "
        f"(budget {agents_index.INDEX_MAX_BYTES})"
    )
    report.lines.append(
        f"  pages: {len(values)}, {sum(values)} bytes in total; min / median / max = "
        f"{values[0]} / {int(statistics.median(values))} / {values[-1]} "
        f"({sizes[-1][1]}; budget {agents_index.PAGE_MAX_BYTES})"
    )


def _new_sources(directory: str, root: Path) -> list[tuple[str, str]]:
    pages_dir = root / directory / agents_index.PAGES_DIR
    sources: list[tuple[str, str]] = []
    for path in sorted(pages_dir.glob("*.md")):
        name = path.relative_to(root).as_posix()
        text = path.read_text(encoding="utf-8")
        if path.name != agents_index.HEAD_NAME:
            front, body = agents_index.split_front_matter(text, name)
            # blank lines stand in for the front matter, so reported lines match the file
            text = "\n" * (len(front) + 2) + body
        sources.append((name, text))
    return sources


def _index_is_stale(directory: str, root: Path) -> list[str]:
    index = root / directory / agents_index.INDEX_NAME
    rendered = agents_index.render(directory, root)
    current = index.read_text(encoding="utf-8") if index.is_file() else None
    if current == rendered:
        return []
    return [agents_index.stale_message(directory, current, rendered).splitlines()[0]]


def check(old_text: str, directory: str, root: Path = ROOT) -> Report:
    """Compare ``old_text`` with the split sources of ``directory``."""

    report = Report()
    agents_index.load_pages(root / directory / agents_index.PAGES_DIR, root)
    old_name = posixpath.join(directory, agents_index.INDEX_NAME) + " (old)"
    pages_base = posixpath.join(directory, agents_index.PAGES_DIR)
    old_units = split_units(old_text, old_name)
    sources = _new_sources(directory, root)
    new_units = [unit for name, text in sources for unit in split_units(text, name)]
    report.lines.append(f"agents migration: {directory}")
    report.lines.append(f"  old {agents_index.INDEX_NAME}: {len(old_text.encode('utf-8'))} bytes")
    _sizes(directory, root, report)
    old_content = _content(old_units, directory)
    new_content = _content(new_units, pages_base)
    report.lines.append(f"  content units: {len(old_content)} old, {len(new_content)} new")
    report.finding(
        "same-file anchor links in the old file (unsupported)",
        sorted(set(ANCHOR_LINK_RE.findall(old_text))),
    )
    compare_units(old_content, new_content, report)
    new_headings = {heading_text(unit) for unit in new_units if unit.kind == HEADING}
    report.finding(
        "headings missing",
        [
            _snippet(unit)
            for unit in old_units
            if unit.kind == HEADING and heading_text(unit) not in new_headings
        ],
    )
    new_text = "\n".join(canonical_text(text, pages_base) for _, text in sources)
    report.finding("tokens missing", missing_tokens(canonical_text(old_text, directory), new_text))
    report.finding("table rows without a header", [_snippet(u) for u in headerless_rows(new_units)])
    report.finding("stale index", _index_is_stale(directory, root))
    return report


def read_old(ref: str | None, file: Path | None, directory: str, root: Path) -> str:
    """Return the old file's text from a Git revision or from a copy on disk."""

    if file is not None:
        return file.read_text(encoding="utf-8")
    environment = git_env.drop()
    result = run_command(
        ("git", "-C", str(root), "show", f"{ref}:{directory}/{agents_index.INDEX_NAME}"),
        allowed_executables=("git",),
        env=environment,
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=(__doc__ or "").splitlines()[0])
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--old-ref", help="Git revision that still holds the unsplit file")
    source.add_argument("--old-file", type=Path, help="copy of the unsplit file")
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("directory")
    args = parser.parse_args(argv)
    root = args.root.resolve()
    directory = args.directory.strip("/")
    try:
        report = check(read_old(args.old_ref, args.old_file, directory, root), directory, root)
    except agents_index.AgentsIndexError as exc:
        print(f"agents migration: {exc}", file=sys.stderr)
        return agents_index.EXIT_INVALID
    print("\n".join(report.lines))
    print("OK: nothing lost" if not report.failures else f"FAIL: {report.failures} finding(s)")
    return 1 if report.failures else 0


if __name__ == "__main__":
    sys.exit(main())
