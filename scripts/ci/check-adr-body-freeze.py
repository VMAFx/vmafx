#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Freeze existing Accepted, Superseded and Deprecated ADR bodies.

Allow status lines, References sections, appended dated status-update sections,
new supersession notes directly below the status block, link target repairs,
whole-line markdownlint directives, and whitespace-only reflow. A commit in the
range may override refusals with an ADR-Body-Edit trailer of at least 20 characters.
Compare ADR changes from the merge base of the supplied revisions.
"""

from __future__ import annotations

import argparse
import difflib
import importlib
import importlib.util
import re
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
run = importlib.import_module("scripts.lib.safe_subprocess").run
GIT = shutil.which("git") or "git"
SECTION_LEVEL = 2
UPDATE_LEVEL = 3
NOTE_MIN_LINES = 2
REASON_MIN_LENGTH = 20

spec = importlib.util.spec_from_file_location(
    "adr_status_drift", Path(__file__).with_name("check-adr-status-drift.py")
)
if spec is None or spec.loader is None:
    raise RuntimeError("cannot load ADR status parser")
status_module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(status_module)
STATUS_RE = status_module.STATUS_RE
ADR = re.compile(r"^docs/adr/[0-9]{4}-[^/]*\.md$")
HEADING = re.compile(r"^(#{1,6})\s+(.+?)\s*$")
UPDATE = re.compile(r"^#{2,3} Status update \d{4}-\d{2}-\d{2}(?:\b|$)")
DIRECTIVE = re.compile(
    r"^<!--\s*markdownlint-(?:disable|enable|disable-line|disable-next-line|capture|restore)\b[^>]*-->\s*$"
)
LINK = re.compile(r"(\[[^\]\n]*\])\([^\n)]*\)")
MESSAGE = (
    "body of an Accepted ADR is frozen (docs/adr/README.md); add a dated "
    '"### Status update YYYY-MM-DD" section or supersede it with a new ADR'
)


def git(*args: str) -> str:
    """Run a read-only Git command or report its failure."""
    result = run(
        [GIT, *args],
        allowed_executables=(GIT,),
        capture_output=True,
        text=True,
        timeout_seconds=120,
        check=False,
    )
    if result.returncode:
        raise RuntimeError(f"git {' '.join(args)}: {result.stderr.strip()}")
    return str(result.stdout)


def status_index(lines: list[str]) -> int | None:
    """Find the first recognized status line."""
    return next((i for i, line in enumerate(lines) if STATUS_RE.match(line)), None)


def section(lines: list[str], index: int) -> str:
    """Find the nearest section heading at a line or insertion point."""
    return next(
        (lines[i] for i in range(min(index, len(lines) - 1), -1, -1) if HEADING.match(lines[i])),
        "(preamble)",
    )


def in_references(lines: list[str], index: int) -> bool:
    """Return whether a line belongs to a level-two References section."""
    for line in reversed(lines[: index + 1]):
        match = HEADING.match(line)
        if match and len(match.group(1)) <= SECTION_LEVEL:
            return line.strip() == "## References"
    return False


def allowed_update(lines: list[str], start: int, end: int) -> bool:
    """Accept inserted dated sections, including their subordinate content."""
    active = False
    for line in lines[start:end]:
        match = HEADING.match(line)
        if match and len(match.group(1)) <= UPDATE_LEVEL:
            active = bool(UPDATE.match(line))
        if not active and line.strip():
            return False
    return active


def allowed_note(old: list[str], pos: int, added: list[str], status: int) -> bool:
    """Accept a newly inserted note immediately after the status block."""
    if pos <= status or any(
        line.strip() and not re.match(r"^- \*\*[^*]+\*\*:", line) for line in old[status + 1 : pos]
    ):
        return False
    content = [line for line in added if line.strip()]
    return (
        len(content) >= NOTE_MIN_LINES
        and content[0].strip() == "> [!NOTE]"
        and re.match(r"^>\s*(?:\*\*)?(?:Superseded by|Status)\b", content[1]) is not None
        and all(line.startswith(">") for line in content)
    )


def link_repair(before: str, after: str) -> bool:
    """Permit only changes to Markdown link destinations."""
    return before != after and LINK.sub(r"\1()", before) == LINK.sub(r"\1()", after)


def normalized_words(lines: list[str]) -> list[str]:
    """Normalize table spacing before comparing words."""
    normalized = []
    for original in lines:
        line = original
        if line.strip().startswith("|"):
            line = re.sub(r"\s*\|\s*", "|", line.strip())
            if re.fullmatch(r"[|:-]+", line):
                line = re.sub(r"-+", "-", line)
        normalized.append(line)
    return " ".join(normalized).split()


def formatting_only(tag: str, before: list[str], after: list[str]) -> bool:
    """Accept blank-only hunks and word-preserving replacements."""
    return all(not line.strip() for line in before + after) or (
        tag == "replace" and normalized_words(before) == normalized_words(after)
    )


def refused(old: list[str], new: list[str], status: int) -> list[tuple[int, str]]:
    """Return the location and section of each disallowed diff hunk."""
    failures: list[tuple[int, str]] = []
    for tag, a, b, c, d in difflib.SequenceMatcher(None, old, new, autojunk=False).get_opcodes():
        if tag == "equal":
            continue
        before = [line for line in old[a:b] if not DIRECTIVE.fullmatch(line)]
        after = [line for line in new[c:d] if not DIRECTIVE.fullmatch(line)]
        if formatting_only(tag, before, after):
            continue
        if tag == "insert" and (
            (
                in_references(old, max(0, a - 1))
                and not any(
                    (match := HEADING.match(line)) and len(match.group(1)) <= SECTION_LEVEL
                    for line in new[c:d]
                )
            )
            or allowed_update(new, c, d)
            or allowed_note(old, a, new[c:d], status)
        ):
            continue
        if tag != "insert" and all(in_references(old, i) for i in range(a, b)):
            continue
        if (
            tag == "replace"
            and len(before) == len(after)
            and all(
                (old_line == old[status] and STATUS_RE.match(new_line))
                or link_repair(old_line, new_line)
                for old_line, new_line in zip(before, after, strict=True)
            )
        ):
            continue
        failures.append((max(1, c + 1), section(old, a)))
    return failures


def file_problems(base: str, head: str, kind: str, path: str, target: str) -> list[tuple[int, str]]:
    """Compare one existing ADR with its destination."""
    old_text = git("show", f"{base}:{path}")
    new_text = "" if kind == "D" else git("show", f"{head}:{target}")
    if kind.startswith("R"):
        return [(1, "(rename)")] if old_text != new_text else []
    old = old_text.splitlines()
    status = status_index(old)
    if status is None or STATUS_RE.match(old[status]).group(1).lower() not in {
        "accepted",
        "superseded",
        "deprecated",
    }:
        return []
    return refused(old, new_text.splitlines(), status)


class InvalidReason(Exception):
    """The override trailer does not explain its reason sufficiently."""


def override(base: str, head: str) -> tuple[str, str] | None:
    """Find and validate a trailer on a commit in the checked range."""
    output = git("log", "--format=%H%x00%(trailers:key=ADR-Body-Edit)%x00", f"{base}..{head}")
    fields = output.split("\0")
    found = None
    for index in range(0, len(fields) - 1, 2):
        sha, trailers = fields[index].strip(), fields[index + 1].strip()
        for trailer in trailers.splitlines():
            reason = trailer.partition(":")[2].strip()
            if len(reason) < REASON_MIN_LENGTH:
                raise InvalidReason("ADR-Body-Edit reason must contain at least 20 characters")
            if found is None:
                found = (sha[:10], reason)
    return found


def check(base: str, head: str) -> int:
    """Check changed ADRs, including renames, against their base versions."""
    git("rev-parse", "--verify", f"{base}^{{commit}}")
    git("rev-parse", "--verify", f"{head}^{{commit}}")
    merge_base = git("merge-base", base, head).strip()
    if not merge_base:
        raise RuntimeError(f"no merge base for {base} and {head}")
    fields = git("diff", "--name-status", "-z", "-M", merge_base, head, "--", "docs/adr/").split(
        "\0"
    )
    waiver = override(base, head)
    failures: list[str] = []
    cursor = 0
    while cursor < len(fields) - 1:
        kind, path = fields[cursor : cursor + 2]
        cursor += 2
        target = path
        if kind.startswith(("R", "C")):
            target = fields[cursor]
            cursor += 1
        if not ADR.fullmatch(path) or path.endswith("/0000-template.md") or kind == "A":
            continue
        for line, heading in file_problems(merge_base, head, kind, path, target):
            failures.append(f"{target}:{line}: {heading}: {MESSAGE}")
    for failure in failures:
        print(f"note: {failure}" if waiver else failure)
    if failures and waiver:
        print(
            f"ADR body freeze: {len(failures)} hunk(s) accepted under ADR-Body-Edit in {waiver[0]}: {waiver[1]}"
        )
    return int(bool(failures) and not waiver)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", required=True)
    parser.add_argument("--head", required=True)
    args = parser.parse_args()
    try:
        return check(args.base, args.head)
    except InvalidReason as exc:
        print(f"ADR body freeze: {exc}", file=sys.stderr)
        return 1
    except (RuntimeError, OSError, TimeoutError, ValueError) as exc:
        print(f"ADR body freeze: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
