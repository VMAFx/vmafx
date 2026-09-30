#!/usr/bin/env python3
# scripts/dev/resolve-state-md-conflict.py — resolve a docs/state.md rebase
# conflict by a three-way merge of its rows, keyed by bug id.
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
"""Resolve a conflicted ``docs/state.md`` by a three-way merge keyed by bug id.

``docs/state.md`` is deliberately left out of the ``merge=union`` list in
``.gitattributes`` (ADR-0165): its rows MOVE from "## Open bugs" to
"## Recently closed", so keeping both sides duplicates the row and the fixed
bug reads as open forever. ``scripts/ci/check-state-md-rows.sh`` gates that.

Git's own line merge cannot see a move, and "one side wins" is wrong in both
directions: mid-rebase "ours" is master PLUS the branch commits already
replayed, so ours-wins keeps master's stale Open copy of a row the branch
closes, and keeps an earlier branch commit's text of a row a later branch
commit rewrote. This tool instead reads the three versions git keeps for the
conflicted path -- ``:1:`` the merge base, ``:2:`` ours, ``:3:`` theirs (the
commit being replayed) -- so it needs no conflict markers, and merges:

* **Rows and move tombstones, by bug id.** A row is a table line whose first
  cell opens with an id; a tombstone is an HTML comment naming
  ``<id> moved to Recently closed``. Both shapes are the ones
  ``check-state-md-rows.sh`` recognises (``**T-ID**``, ``T-ID``, ``**T7-16**``,
  ``Netflix#NNN``, ``**Netflix/vmaf#NNN**``). Each id's state is its text plus
  the ``## `` section it sits in, or absent. Same on both sides: take it.
  Unchanged on one side: take the other (an edit, a move, a close, a delete).
  Changed differently on both: a conflict.
* **Disposition rows, by label.** A row of the table under
  "## First-release phase classification" whose first cell is a bold label
  (``| **RC3 performance ...** | `T-A`<br>`T-B` | prose |``) is a record keyed
  by that label. When both sides changed it, its id list merges as a set --
  ours, plus the ids theirs added, minus the ids either side removed, in ours'
  order with theirs' additions after -- and every other cell three-way by
  text. Rows repeating a label on one side are first folded into one (union of
  ids), and the tool says so.
* **Every other line** (headings, prose, ``_Updated`` lines, tables without an
  id column) by a line-level three-way merge. Lines both sides add at the same
  place are all kept, theirs after ours, but a non-blank line both sides added
  is kept once wherever each put it (a branch stacked on a PR that master
  already squash-merged replays that PR's ``_Updated`` line), and lines either
  side deleted go even when the two deletions overlap; another table row both
  sides edited is merged cell by cell the same way. Anything else both sides
  changed is a conflict.

Placement keeps ours' order. A line that comes only from theirs goes at its
theirs position, after the nearest preceding theirs line (in the same section)
that is present in the output, and after any lines ours added at that point.

Conflicts write nothing: the tool names every conflicting id, label or line
and exits 1. Rerun with ``--take NAME=ours`` or ``--take NAME=theirs`` for
each: NAME is a bug id (that side's state of its row and tombstone), a
disposition label, or the ``line:N`` handle the report prints for a line.
After writing, the tool runs ``check-state-md-rows.sh`` on the result and
exits 3 if the gate rejects it or if a bug id ends up in two disposition rows.
The file is always written with LF line endings.

Usage, mid-rebase, with docs/state.md conflicted::

    python3 scripts/dev/resolve-state-md-conflict.py docs/state.md
    git add docs/state.md && git rebase --continue

Exit: 0 resolved and the row gate passes; 1 conflicts, nothing written;
2 bad usage, or the path has no unmerged index stages; 3 written, but the row
gate rejects it or a bug id sits in two disposition rows.
"""

from __future__ import annotations

import argparse
import difflib
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import TypeVar

# The id shapes check-state-md-rows.sh extracts; keep the two in step.
ID_PATTERN = r"T-[A-Z0-9._-]+|T[0-9]+-[0-9]+|Netflix(?:/vmaf)?#[0-9]+"
ROW_RE = re.compile(rf"^\| \*{{0,2}}({ID_PATTERN})")
TOMBSTONE_RE = re.compile(rf"({ID_PATTERN})\s+moved to [Rr]ecently [Cc]losed")
# A disposition row names its phase in a bold first cell and lists its bug ids
# in the second: `| **RC3 performance ...** | `T-A`<br>`T-B` | prose |`.
DISPOSITION_SECTION = "First-release phase classification"
DISPOSITION_RE = re.compile(r"^\|\s*\*\*([^*|]+)\*\*\s*\|")
TAKE_RE = re.compile(r"^(.+)=(ours|theirs)$")
CELL_SPLIT_RE = re.compile(r"(?<!\\)\|")
CODE_ITEM_RE = re.compile(r"^`[^`]+`$")
CODE_SPAN_RE = re.compile(r"`([^`]+)`")
LIST_SEPARATOR = "<br>"
LIST_CELL = 2
HEADING = "## "
EXCERPT = 110

CHECKER = Path(__file__).resolve().parents[2] / "scripts" / "ci" / "check-state-md-rows.sh"
GIT_TIMEOUT_SECONDS = 60
CHECK_TIMEOUT_SECONDS = 120

KIND_NAMES = {
    "row": "row",
    "tomb": "moved-to-Recently-closed tombstone",
    "disp": "disposition row",
}

EXIT_OK = 0
EXIT_CONFLICT = 1
EXIT_USAGE = 2
EXIT_GATE = 3

Key = tuple[str, str, int]  # (kind "row" | "tomb" | "disp", bug id or label, occurrence)
State = tuple[str, str] | None  # (line text, section) or absent
T = TypeVar("T")


class UsageError(Exception):
    """The invocation or the repository state cannot be resolved."""


class PlacementError(Exception):
    """A line from theirs has no neighbour left in the output to sit beside."""


@dataclass(frozen=True)
class Line:
    """One line of one version: its 1-based number, section and id key."""

    text: str
    section: str
    key: Key | None
    number: int


@dataclass(frozen=True)
class Hunk:
    """A change of base plain lines [b1, b2) into side plain lines [s1, s2)."""

    b1: int
    b2: int
    s1: int
    s2: int


@dataclass
class PlainSide:
    """One side's id-free lines, aligned against the base's."""

    positions: list[int]
    texts: list[str]
    hunks: list[Hunk]
    matched: dict[int, int]
    plain_of: dict[int, int]


@dataclass(frozen=True)
class LineConflict:
    """Overlapping, different changes to the same base lines."""

    handle: str
    ours: Hunk
    theirs: Hunk


@dataclass
class PlainMerge:
    """The outcome of the line-level three-way merge."""

    removed_base: set[int] = field(default_factory=set)
    dropped_ours: set[int] = field(default_factory=set)
    inserted_theirs: set[int] = field(default_factory=set)
    alias: dict[int, int] = field(default_factory=dict)
    merged_text: dict[int, str] = field(default_factory=dict)
    conflicts: list[LineConflict] = field(default_factory=list)
    used_takes: set[str] = field(default_factory=set)


@dataclass(frozen=True)
class Versions:
    """The three index stages of the conflicted path, parsed."""

    base: list[Line]
    ours: list[Line]
    theirs: list[Line]


@dataclass(frozen=True)
class OutLine:
    """A line of the result and the identity it is matched by."""

    ident: str
    text: str


@dataclass
class Resolution:
    """Everything the merge decided."""

    text: str = ""
    key_conflicts: list[Key] = field(default_factory=list)
    line_conflicts: list[LineConflict] = field(default_factory=list)
    from_theirs: list[str] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)


def merge3(base: T, ours: T, theirs: T) -> tuple[T, bool]:
    """Three-way pick: the changed side wins; both changed differently fails."""
    if theirs in (ours, base):
        return ours, True
    if ours == base:
        return theirs, True
    return ours, False


def line_kind(text: str, section: str) -> tuple[str, str] | None:
    """Return (kind, id or label) for a keyed line, else None.

    Kinds: "row" (a bug row), "tomb" (a move tombstone), "disp" (a row of the
    disposition table, keyed by its bold label).
    """
    row = ROW_RE.match(text)
    if row:
        return ("row", row.group(1))
    if text.startswith("<!--"):
        tomb = TOMBSTONE_RE.search(text)
        if tomb:
            return ("tomb", tomb.group(1))
    disposition = DISPOSITION_RE.match(text) if section == DISPOSITION_SECTION else None
    if disposition:
        return ("disp", disposition.group(1).strip())
    return None


def parse_version(text: str) -> list[Line]:
    """Split a state.md body into lines tagged with section and id key."""
    raw = text.split("\n")
    if raw and raw[-1] == "":
        raw.pop()
    section = ""
    seen: dict[tuple[str, str], int] = {}
    lines: list[Line] = []
    for number, item in enumerate(raw, start=1):
        body = item.removesuffix("\r")
        if body.startswith(HEADING):
            section = body[len(HEADING) :].rstrip()
        kind = line_kind(body, section)
        key: Key | None = None
        if kind is not None:
            seen[kind] = seen.get(kind, 0) + 1
            key = (kind[0], kind[1], seen[kind])
        lines.append(Line(body, section, key, number))
    return lines


def fold_disposition(first: str, duplicate: str) -> tuple[str, bool]:
    """Fold a duplicate disposition row into the first: union of listed ids.

    Returns the folded row and whether the other cells already agreed.
    """
    cells, extra = split_cells(first), split_cells(duplicate)
    if len(cells) != len(extra) or len(cells) <= LIST_CELL:
        return first, False
    kept = list_items(cells[LIST_CELL]) or []
    union = merge_items([], kept, list_items(extra[LIST_CELL]) or [])
    if union != kept:
        cells[LIST_CELL] = f" {LIST_SEPARATOR.join(union)} "
    agree = without_list(cells) == without_list(extra)
    return "|".join(cells), agree


def without_list(cells: list[str]) -> list[str]:
    """A disposition row's cells other than its id list."""
    return cells[:LIST_CELL] + cells[LIST_CELL + 1 :]


def collapse_dispositions(lines: list[Line], side: str) -> tuple[list[Line], list[str]]:
    """Merge disposition rows that repeat a label into the first such row."""
    first: dict[str, int] = {}
    out: list[Line] = []
    notes: list[str] = []
    for line in lines:
        if line.key is None or line.key[0] != "disp":
            out.append(line)
            continue
        label = line.key[1]
        if label not in first:
            first[label] = len(out)
            out.append(line)
            continue
        kept = out[first[label]]
        text, agree = fold_disposition(kept.text, line.text)
        out[first[label]] = Line(text, kept.section, kept.key, kept.number)
        rest = "" if agree else "; its other cells differed, the first row's were kept"
        notes.append(
            f"collapsed the duplicate '{label}' row at {side} line {line.number} into "
            f"line {kept.number} (union of ids{rest})"
        )
    return out, notes


def keyed_index(lines: list[Line]) -> dict[Key, Line]:
    """Map every row and tombstone key of a version to its line."""
    return {line.key: line for line in lines if line.key is not None}


def state_of(index: dict[Key, Line], key: Key) -> State:
    """Return the (text, section) state of a key, or None when absent."""
    line = index.get(key)
    return None if line is None else (line.text, line.section)


def decide_keys(versions: Versions, takes: dict[str, str]) -> tuple[dict[Key, State], list[Key]]:
    """Decide every keyed line (row, tombstone, disposition); finals and conflicts."""
    indexes = [keyed_index(v) for v in (versions.base, versions.ours, versions.theirs)]
    keys = sorted(set(indexes[0]) | set(indexes[1]) | set(indexes[2]))
    finals: dict[Key, State] = {}
    conflicts: list[Key] = []
    for key in keys:
        base, ours, theirs = (state_of(index, key) for index in indexes)
        side = takes.get(key[1])
        if side is not None:
            finals[key] = ours if side == "ours" else theirs
            continue
        merged, ok = merge3(base, ours, theirs)
        if not ok and key[0] == "disp":
            merged, ok = merge_disposition(base, ours, theirs)
        if not ok:
            conflicts.append(key)
        finals[key] = merged
    return finals, conflicts


def merge_disposition(base: State, ours: State, theirs: State) -> tuple[State, bool]:
    """Merge a disposition row both sides edited: id list as a set, cells by text."""
    if ours is None or theirs is None or ours[1] != theirs[1]:
        return ours, False
    if base is None:
        cells = split_cells(ours[0])
        base_text = "|".join(cells[:2] + [""] * (len(cells) - 2))
    else:
        base_text = base[0]
    text = merge_row_cells(base_text, ours[0], theirs[0])
    if text is None:
        return ours, False
    return (text, ours[1]), True


def split_hunk(b1: int, b2: int, s1: int, s2: int) -> list[Hunk]:
    """Split an n-line to n-line replacement into n single-line edits."""
    count = b2 - b1
    if count > 1 and count == s2 - s1:
        return [Hunk(b1 + k, b1 + k + 1, s1 + k, s1 + k + 1) for k in range(count)]
    return [Hunk(b1, b2, s1, s2)]


def plain_lines(lines: list[Line]) -> tuple[list[int], list[str]]:
    """Positions and texts of a version's id-free lines."""
    positions = [pos for pos, line in enumerate(lines) if line.key is None]
    return positions, [lines[pos].text for pos in positions]


def plain_side(base_texts: list[str], lines: list[Line]) -> PlainSide:
    """Align a side's id-free lines against the base's id-free lines."""
    positions, texts = plain_lines(lines)
    matcher = difflib.SequenceMatcher(None, base_texts, texts, autojunk=False)
    hunks: list[Hunk] = []
    matched: dict[int, int] = {}
    for tag, b1, b2, s1, s2 in matcher.get_opcodes():
        if tag == "equal":
            matched.update({s1 + k: b1 + k for k in range(b2 - b1)})
        else:
            hunks.extend(split_hunk(b1, b2, s1, s2))
    plain_of = {pos: k for k, pos in enumerate(positions)}
    return PlainSide(positions, texts, hunks, matched, plain_of)


def overlaps(ours: Hunk, theirs: Hunk) -> bool:
    """True when two changes touch the same base lines (insertions: interior)."""
    ours_insert = ours.b1 == ours.b2
    theirs_insert = theirs.b1 == theirs.b2
    if ours_insert and theirs_insert:
        return False
    if ours_insert:
        return theirs.b1 < ours.b1 < theirs.b2
    if theirs_insert:
        return ours.b1 < theirs.b1 < ours.b2
    return ours.b1 < theirs.b2 and theirs.b1 < ours.b2


def split_cells(text: str) -> list[str]:
    """Split a table row on unescaped pipes (``\\|`` stays inside its cell)."""
    return CELL_SPLIT_RE.split(text)


def list_items(cell: str) -> list[str] | None:
    """Return a ``<br>`` list of code spans as items, or None for other cells."""
    items = [part.strip() for part in cell.split(LIST_SEPARATOR)]
    items = [item for item in items if item]
    if all(CODE_ITEM_RE.match(item) for item in items):
        return items
    return None


def merge_items(base: list[str], ours: list[str], theirs: list[str]) -> list[str]:
    """Set three-way merge: ours + (theirs - base) - (base - theirs), in ours' order."""
    kept = [item for item in ours if item in theirs or item not in base]
    return kept + [item for item in theirs if item not in base and item not in kept]


def merge_list_cell(base: str, ours: str, theirs: str) -> str | None:
    """Three-way merge of a ``<br>`` list cell, item by item; None if not one.

    A base cell that is no list (``none remain``) counts as the empty list.
    """
    ours_items, theirs_items = list_items(ours), list_items(theirs)
    if ours_items is None or theirs_items is None:
        return None
    kept = merge_items(list_items(base) or [], ours_items, theirs_items)
    if not kept:
        return ours if not ours_items else " "
    return f" {LIST_SEPARATOR.join(kept)} "


def merge_cell(base: str, ours: str, theirs: str) -> str | None:
    """Three-way merge of one table cell; a list cell merges item by item."""
    cell, ok = merge3(base, ours, theirs)
    return cell if ok else merge_list_cell(base, ours, theirs)


def comparable_rows(texts: tuple[str, str, str], cells: list[list[str]]) -> bool:
    """True when three lines are versions of one table row (same shape and key cell)."""
    if not all(text.startswith("|") for text in texts):
        return False
    return len({len(row) for row in cells}) == 1 and len({row[1] for row in cells}) == 1


def merge_row_cells(base: str, ours: str, theirs: str) -> str | None:
    """Merge a table row both sides edited, cell by cell; None on a clash."""
    cells = [split_cells(text) for text in (base, ours, theirs)]
    if not comparable_rows((base, ours, theirs), cells):
        return None
    merged: list[str] = []
    for base_cell, ours_cell, theirs_cell in zip(*cells, strict=True):
        cell = merge_cell(base_cell, ours_cell, theirs_cell)
        if cell is None:
            return None
        merged.append(cell)
    return "|".join(merged)


def base_line_number(base_numbers: list[int], index: int) -> int:
    """1-based line number in the base stage of base plain line `index`."""
    if index < len(base_numbers):
        return base_numbers[index]
    return (base_numbers[-1] + 1) if base_numbers else 1


def same_insertion(ours: Hunk, theirs: Hunk) -> bool:
    """True when both sides insert at the same base point."""
    return ours.b1 == ours.b2 == theirs.b1 == theirs.b2


@dataclass(frozen=True)
class PlainInputs:
    """The aligned id-free lines of all three versions."""

    base_texts: list[str]
    base_numbers: list[int]
    ours: PlainSide
    theirs: PlainSide


def alias_hunk(merge: PlainMerge, ours: Hunk, theirs: Hunk) -> None:
    """Record that theirs' lines of a hunk are the same lines as ours'."""
    for k in range(theirs.s2 - theirs.s1):
        merge.alias[theirs.s1 + k] = ours.s1 + k


def try_combine(inputs: PlainInputs, merge: PlainMerge, ours: Hunk, theirs: Hunk) -> bool:
    """Absorb theirs' hunk into ours' when identical or cell-mergeable."""
    ours_text = inputs.ours.texts[ours.s1 : ours.s2]
    theirs_text = inputs.theirs.texts[theirs.s1 : theirs.s2]
    same_range = ours.b1 == theirs.b1 and ours.b2 == theirs.b2
    if same_range and ours_text == theirs_text:
        alias_hunk(merge, ours, theirs)
        return True
    single = ours.b2 - ours.b1 == 1 and len(ours_text) == 1 and len(theirs_text) == 1
    if not (same_range and single):
        return False
    merged = merge_row_cells(inputs.base_texts[ours.b1], ours_text[0], theirs_text[0])
    if merged is None:
        return False
    alias_hunk(merge, ours, theirs)
    merge.merged_text[ours.s1] = merged
    return True


def apply_take(merge: PlainMerge, ours: Hunk, theirs: Hunk, side: str) -> None:
    """Resolve a line conflict over one base range in favour of `side`."""
    if side == "theirs":
        merge.dropped_ours.update(range(ours.s1, ours.s2))
        merge.inserted_theirs.update(range(theirs.s1, theirs.s2))


def settle_clash(
    inputs: PlainInputs, merge: PlainMerge, clash: list[Hunk], theirs: Hunk, takes: dict[str, str]
) -> None:
    """Settle a theirs hunk that overlaps ours' changes, or record a conflict.

    Overlapping pure deletions never conflict: every line either side deleted
    goes, the other side having left it unchanged or deleted it too.
    """
    if theirs.s1 == theirs.s2 and all(hunk.s1 == hunk.s2 for hunk in clash):
        return
    if len(clash) == 1 and try_combine(inputs, merge, clash[0], theirs):
        return
    handle = f"line:{base_line_number(inputs.base_numbers, theirs.b1)}"
    side = takes.get(handle)
    same_range = len(clash) == 1 and (clash[0].b1, clash[0].b2) == (theirs.b1, theirs.b2)
    if side is not None and same_range:
        merge.used_takes.add(handle)
        apply_take(merge, clash[0], theirs, side)
        return
    merge.conflicts.extend(LineConflict(handle, hunk, theirs) for hunk in clash)


def merge_plain(inputs: PlainInputs, takes: dict[str, str]) -> PlainMerge:
    """Three-way merge of every line that carries no bug id."""
    merge = PlainMerge()
    for theirs in inputs.theirs.hunks:
        merge.removed_base.update(range(theirs.b1, theirs.b2))
        clash = [hunk for hunk in inputs.ours.hunks if overlaps(hunk, theirs)]
        if clash:
            settle_clash(inputs, merge, clash, theirs, takes)
            continue
        twins = [hunk for hunk in inputs.ours.hunks if same_insertion(hunk, theirs)]
        if twins and try_combine(inputs, merge, twins[0], theirs):
            continue
        merge.inserted_theirs.update(range(theirs.s1, theirs.s2))
    alias_identical_additions(inputs, merge)
    return merge


def added_lines(side: PlainSide) -> dict[str, list[int]]:
    """Text -> indices of the lines a side added (not aligned with the base)."""
    added: dict[str, list[int]] = {}
    for k, text in enumerate(side.texts):
        if k not in side.matched and text.strip():
            added.setdefault(text, []).append(k)
    return added


def alias_identical_additions(inputs: PlainInputs, merge: PlainMerge) -> None:
    """Keep once a non-blank line both sides added, wherever each put it.

    A branch stacked on another PR replays that PR's commits, which master
    already carries as a squash merge: both sides then add the same
    ``_Updated`` line, often at different places.
    """
    ours_added = added_lines(inputs.ours)
    base_texts = set(inputs.base_texts)
    for text, theirs_at in added_lines(inputs.theirs).items():
        ours_at = ours_added.get(text, [])
        if len(ours_at) != 1 or len(theirs_at) != 1 or text in base_texts:
            continue
        mine, yours = ours_at[0], theirs_at[0]
        if yours in merge.inserted_theirs and mine not in merge.dropped_ours:
            merge.inserted_theirs.discard(yours)
            merge.alias[yours] = mine


def key_ident(key: Key) -> str:
    """Identity of a row or tombstone across versions."""
    return f"K:{key[0]}:{key[1]}:{key[2]}"


def side_identities(
    lines: list[Line], side: PlainSide, alias: dict[int, int], tag: str
) -> list[str]:
    """Identity of every line of one side: key, base line, or side-new line."""
    idents: list[str] = []
    for pos, line in enumerate(lines):
        if line.key is not None:
            idents.append(key_ident(line.key))
            continue
        k = side.plain_of[pos]
        if k in side.matched:
            idents.append(f"B:{side.matched[k]}")
        elif k in alias:
            idents.append(f"O:{alias[k]}")
        else:
            idents.append(f"{tag}:{k}")
    return idents


def placement(key: Key, final: State, ours_index: dict[Key, Line]) -> str:
    """Where a decided key goes: 'ours' location, 'theirs' location, or 'drop'."""
    if final is None:
        return "drop"
    ours = ours_index.get(key)
    if ours is not None and ours.section == final[1]:
        return "ours"
    return "theirs"


@dataclass(frozen=True)
class Plan:
    """The decisions the output is assembled from."""

    versions: Versions
    finals: dict[Key, State]
    plain: PlainMerge
    inputs: PlainInputs
    ours_ids: list[str]
    theirs_ids: list[str]


def keyed_output_line(plan: Plan, key: Key, ours_index: dict[Key, Line], where: str) -> str | None:
    """Text of a row or tombstone if its decided location is `where`."""
    final = plan.finals[key]
    if final is None or placement(key, final, ours_index) != where:
        return None
    return final[0]


def ours_output_line(plan: Plan, pos: int, ours_index: dict[Key, Line]) -> str | None:
    """Text ours' line `pos` contributes to the output, or None if dropped."""
    line = plan.versions.ours[pos]
    if line.key is not None:
        return keyed_output_line(plan, line.key, ours_index, "ours")
    k = plan.inputs.ours.plain_of[pos]
    base = plan.inputs.ours.matched.get(k)
    if base is not None and base in plan.plain.removed_base:
        return None
    if k in plan.plain.dropped_ours:
        return None
    return plan.plain.merged_text.get(k, line.text)


def theirs_output_line(plan: Plan, pos: int, ours_index: dict[Key, Line]) -> str | None:
    """Text theirs' line `pos` inserts into the output, or None if it does not."""
    line = plan.versions.theirs[pos]
    if line.key is not None:
        return keyed_output_line(plan, line.key, ours_index, "theirs")
    k = plan.inputs.theirs.plain_of[pos]
    if plan.theirs_ids[pos].startswith("T:") and k in plan.plain.inserted_theirs:
        return line.text
    return None


def section_at(out: list[OutLine], index: int) -> str:
    """The ``## `` section output line `index` sits in."""
    for back in range(index, -1, -1):
        if out[back].text.startswith(HEADING):
            return out[back].text[len(HEADING) :].rstrip()
    return ""


TheirsSet = set[tuple[str, str]]  # (identity, section) of every theirs line


def skip_ours_only(out: list[OutLine], index: int, theirs_set: TheirsSet, section: str) -> int:
    """Advance past lines theirs lacks in this section, so theirs lands after ours."""
    while index < len(out):
        line = out[index]
        if (line.ident, section) in theirs_set or line.text.startswith(HEADING):
            break
        index += 1
    return index


@dataclass(frozen=True)
class AnchorSearch:
    """What a neighbour search for one theirs line looks at."""

    out: list[OutLine]
    where: dict[str, int]
    theirs: list[Line]
    theirs_ids: list[str]
    section: str


def present_neighbour(search: AnchorSearch, order: range) -> int | None:
    """Output index of the first theirs line in `order` present in the section."""
    for pos in order:
        if search.theirs[pos].section != search.section:
            return None
        k = search.where.get(search.theirs_ids[pos])
        if k is not None and section_at(search.out, k) == search.section:
            return k
    return None


def anchor_index(out: list[OutLine], plan: Plan, pos: int, theirs_set: TheirsSet) -> int:
    """Output index for theirs' line `pos`, by its nearest present neighbour.

    A heading anchors on the section before it: it is that section's end.
    """
    theirs = plan.versions.theirs
    owner = pos - 1 if theirs[pos].text.startswith(HEADING) and pos > 0 else pos
    where = {line.ident: k for k, line in enumerate(out)}
    search = AnchorSearch(out, where, theirs, plan.theirs_ids, theirs[owner].section)
    before = present_neighbour(search, range(pos - 1, -1, -1))
    if before is not None:
        return skip_ours_only(out, before + 1, theirs_set, search.section)
    after = present_neighbour(search, range(pos + 1, len(theirs)))
    if after is not None:
        return after
    raise PlacementError(
        f"no neighbour left in '## {search.section}' for: {theirs[pos].text[:EXCERPT]}"
    )


def assemble(plan: Plan) -> list[OutLine]:
    """Ours' lines in ours' order, then theirs' lines at their anchors."""
    ours_index = keyed_index(plan.versions.ours)
    out: list[OutLine] = []
    for pos in range(len(plan.versions.ours)):
        text = ours_output_line(plan, pos, ours_index)
        if text is not None:
            out.append(OutLine(plan.ours_ids[pos], text))
    theirs_set = {
        (ident, line.section)
        for ident, line in zip(plan.theirs_ids, plan.versions.theirs, strict=True)
    }
    for pos in range(len(plan.versions.theirs)):
        text = theirs_output_line(plan, pos, ours_index)
        if text is not None:
            out.insert(
                anchor_index(out, plan, pos, theirs_set), OutLine(plan.theirs_ids[pos], text)
            )
    return out


def theirs_changes(versions: Versions, finals: dict[Key, State]) -> list[str]:
    """Ids whose merged state came from theirs, for the summary line."""
    ours_index = keyed_index(versions.ours)
    changed = {key[1] for key, final in finals.items() if final != state_of(ours_index, key)}
    return sorted(changed)


def plan_merge(versions: Versions, takes: dict[str, str]) -> tuple[Plan, list[Key]]:
    """Run both merges and derive the identities the assembly matches on."""
    finals, key_conflicts = decide_keys(versions, takes)
    base_positions, base_texts = plain_lines(versions.base)
    inputs = PlainInputs(
        base_texts,
        [versions.base[pos].number for pos in base_positions],
        plain_side(base_texts, versions.ours),
        plain_side(base_texts, versions.theirs),
    )
    plain = merge_plain(inputs, takes)
    ours_ids = side_identities(versions.ours, inputs.ours, {}, "O")
    theirs_ids = side_identities(versions.theirs, inputs.theirs, plain.alias, "T")
    plan = Plan(versions, finals, plain, inputs, ours_ids, theirs_ids)
    return plan, key_conflicts


def check_takes(versions: Versions, plain: PlainMerge, takes: dict[str, str]) -> None:
    """Reject a --take naming no id, no disposition label and no conflicting line."""
    ids = {
        line.key[1]
        for lines in (versions.base, versions.ours, versions.theirs)
        for line in lines
        if line.key is not None
    }
    unused = [
        name
        for name in takes
        if name not in ids
        and name not in plain.used_takes
        and not any(c.handle == name for c in plain.conflicts)
    ]
    if unused:
        raise UsageError(f"--take names nothing in the conflicted file: {', '.join(unused)}")


def parse_stages(base: str, ours: str, theirs: str) -> tuple[Versions, list[str]]:
    """Parse the three stages and collapse repeated disposition rows on each."""
    notes: list[str] = []
    parsed: list[list[Line]] = []
    for side, text in (("base", base), ("ours", ours), ("theirs", theirs)):
        lines, side_notes = collapse_dispositions(parse_version(text), side)
        parsed.append(lines)
        notes.extend(side_notes)
    return Versions(parsed[0], parsed[1], parsed[2]), notes


def double_listed(lines: list[Line]) -> list[str]:
    """Warnings for bug ids listed in more than one disposition row."""
    rows: dict[str, list[str]] = {}
    for line in lines:
        if line.key is None or line.key[0] != "disp":
            continue
        cells = split_cells(line.text)
        listed = CODE_SPAN_RE.findall(cells[LIST_CELL]) if len(cells) > LIST_CELL else []
        for bug in dict.fromkeys(listed):
            rows.setdefault(bug, []).append(line.key[1])
    return [
        f"{bug} is listed in {len(labels)} disposition rows: " + "; ".join(labels)
        for bug, labels in rows.items()
        if len(labels) > 1
    ]


def resolve(base: str, ours: str, theirs: str, takes: dict[str, str]) -> Resolution:
    """Merge the three stage bodies; the text is empty when anything conflicts."""
    versions, notes = parse_stages(base, ours, theirs)
    plan, key_conflicts = plan_merge(versions, takes)
    check_takes(versions, plan.plain, takes)
    result = Resolution(key_conflicts=key_conflicts, line_conflicts=plan.plain.conflicts)
    result.notes = notes
    if key_conflicts or plan.plain.conflicts:
        return result
    out = assemble(plan)
    newline = "\n" if (ours or theirs).endswith("\n") else ""
    result.text = "\n".join(line.text for line in out) + newline
    result.from_theirs = theirs_changes(versions, plan.finals)
    result.warnings = double_listed(parse_version(result.text))
    return result


def describe_state(label: str, state: State) -> str:
    """One report line for one side's state of a key."""
    if state is None:
        return f"    {label:<7} absent"
    return f"    {label:<7} (## {state[1]}) {state[0][:EXCERPT]}"


def report_key_conflicts(versions: Versions, keys: list[Key]) -> None:
    """Print each conflicting id with its three states and the way out."""
    indexes = [keyed_index(v) for v in (versions.base, versions.ours, versions.theirs)]
    for key in keys:
        kind = KIND_NAMES[key[0]]
        print(f"  {key[1]} ({kind}): both sides changed it since the merge base", file=sys.stderr)
        for label, index in zip(("base", "ours", "theirs"), indexes, strict=True):
            print(describe_state(label, state_of(index, key)), file=sys.stderr)
        name = key[1] if key[0] != "disp" else f"'{key[1]}"
        close = "" if key[0] != "disp" else "'"
        print(
            f"    rerun with --take {name}=ours{close} or --take {name}=theirs{close}",
            file=sys.stderr,
        )


def report_line_conflicts(
    texts: tuple[list[str], list[str], list[str]], conflicts: list[LineConflict]
) -> None:
    """Print each conflicting line range with both sides' replacement."""
    base_texts, ours_texts, theirs_texts = texts
    for conflict in conflicts:
        print(f"  {conflict.handle}: both sides changed these lines", file=sys.stderr)
        end = max(conflict.theirs.b2, conflict.ours.b2)
        spans = (
            ("base", base_texts[min(conflict.theirs.b1, conflict.ours.b1) : end]),
            ("ours", ours_texts[conflict.ours.s1 : conflict.ours.s2]),
            ("theirs", theirs_texts[conflict.theirs.s1 : conflict.theirs.s2]),
        )
        for label, lines in spans:
            shown = " / ".join(line[:EXCERPT] for line in lines[:3]) or "(nothing)"
            print(f"    {label:<7} {shown}", file=sys.stderr)
        print(
            f"    rerun with --take {conflict.handle}=ours or --take {conflict.handle}=theirs",
            file=sys.stderr,
        )


def report_conflicts(path: str, base: str, ours: str, theirs: str, result: Resolution) -> None:
    """Explain why nothing was written."""
    versions, _ = parse_stages(base, ours, theirs)
    count = len(result.key_conflicts) + len(result.line_conflicts)
    print(f"resolve-state-md-conflict: {count} conflict(s); {path} NOT written", file=sys.stderr)
    report_key_conflicts(versions, result.key_conflicts)
    plain = [plain_lines(v)[1] for v in (versions.base, versions.ours, versions.theirs)]
    report_line_conflicts((plain[0], plain[1], plain[2]), result.line_conflicts)
    print(
        "  'ours' is the side being rebased onto (master plus the commits already\n"
        "  replayed); 'theirs' is the commit being replayed.",
        file=sys.stderr,
    )


def run_git(git: str, args: list[str]) -> bytes:
    """Run a read-only git command and return its stdout."""
    proc = subprocess.run(  # noqa: S603 -- fixed git binary, fixed read-only subcommands
        [git, *args], capture_output=True, timeout=GIT_TIMEOUT_SECONDS, check=False
    )
    if proc.returncode != 0:
        detail = proc.stderr.decode("utf-8", "replace").strip()
        raise UsageError(f"git {' '.join(args[:2])} failed: {detail}")
    return proc.stdout


def read_stages(path: str) -> dict[str, str]:
    """Read the base / ours / theirs index stages git keeps for `path`."""
    git = shutil.which("git")
    if git is None:
        raise UsageError("git is not on PATH")
    listing = run_git(git, ["ls-files", "-u", "-z", "--", path]).decode("utf-8")
    shas: dict[str, str] = {}
    names: set[str] = set()
    for record in filter(None, listing.split("\0")):
        meta, _, name = record.partition("\t")
        fields = meta.split()
        shas[fields[2]] = fields[1]
        names.add(name)
    if not shas:
        raise UsageError(
            f"{path} has no unmerged index stages: run this while git reports it conflicted"
        )
    if len(names) != 1 or "2" not in shas or "3" not in shas:
        raise UsageError(f"{path}: expected one file changed on both sides; resolve by hand")
    stages = {"1": ""}
    for stage, sha in shas.items():
        stages[stage] = run_git(git, ["cat-file", "blob", sha]).decode("utf-8")
    return stages


def run_gate(path: Path) -> int:
    """Run the row gate on the written file; its output goes to the terminal."""
    bash = shutil.which("bash")
    if bash is None or not CHECKER.is_file():
        print(f"resolve-state-md-conflict: cannot run {CHECKER}; verify by hand", file=sys.stderr)
        return EXIT_GATE
    proc = subprocess.run(  # noqa: S603 -- fixed bash binary running the in-repo gate
        [bash, str(CHECKER), str(path)], timeout=CHECK_TIMEOUT_SECONDS, check=False
    )
    if proc.returncode != 0:
        print(
            f"resolve-state-md-conflict: WROTE {path}, BUT THE ROW GATE REJECTS IT -- "
            "fix the rows above before 'git add'",
            file=sys.stderr,
        )
        return EXIT_GATE
    return EXIT_OK


def parse_args(argv: list[str]) -> argparse.Namespace:
    """Parse the command line; argparse exits 2 on bad usage."""
    parser = argparse.ArgumentParser(
        prog="resolve-state-md-conflict.py",
        description="Resolve a conflicted docs/state.md three-way, row by bug id.",
    )
    parser.add_argument("path", help="the conflicted file, normally docs/state.md")
    parser.add_argument(
        "--take",
        action="append",
        default=[],
        metavar="NAME=SIDE",
        help="settle a reported conflict: NAME is a bug id, a disposition label or a "
        "line:N handle; SIDE is ours or theirs",
    )
    return parser.parse_args(argv)


def parse_takes(values: list[str]) -> dict[str, str]:
    """Parse --take NAME=SIDE values."""
    takes: dict[str, str] = {}
    for value in values:
        match = TAKE_RE.match(value)
        if match is None:
            raise UsageError(f"--take {value!r}: expected NAME=ours or NAME=theirs")
        takes[match.group(1)] = match.group(2)
    return takes


def main(argv: list[str]) -> int:
    """Resolve the conflicted file named on the command line."""
    args = parse_args(argv)
    try:
        takes = parse_takes(args.take)
        stages = read_stages(args.path)
        result = resolve(stages["1"], stages["2"], stages["3"], takes)
    except (UsageError, UnicodeDecodeError) as exc:
        print(f"resolve-state-md-conflict: {exc}", file=sys.stderr)
        return EXIT_USAGE
    except PlacementError as exc:
        print(f"resolve-state-md-conflict: {args.path} NOT written: {exc}", file=sys.stderr)
        return EXIT_CONFLICT
    if result.key_conflicts or result.line_conflicts:
        report_conflicts(args.path, stages["1"], stages["2"], stages["3"], result)
        return EXIT_CONFLICT
    target = Path(args.path)
    target.write_bytes(result.text.encode("utf-8"))
    taken = ", ".join(result.from_theirs) or "none"
    print(
        f"resolve-state-md-conflict: wrote {target}; rows taken from the replayed commit: {taken}"
    )
    for note in result.notes:
        print(f"resolve-state-md-conflict: {note}")
    sys.stdout.flush()
    status = run_gate(target)
    for warning in result.warnings:
        print(f"resolve-state-md-conflict: WARNING: {warning}", file=sys.stderr)
    if result.warnings:
        print(
            f"resolve-state-md-conflict: WROTE {target}, BUT A BUG ID SITS IN TWO DISPOSITION "
            "ROWS -- keep it in one before 'git add'",
            file=sys.stderr,
        )
        return EXIT_GATE
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
