# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

"""Tracker abstraction for `.workingdir/BACKLOG.md` and GitHub PRs.

Symphony §3.1 / §4.1.1 normalises the issue tracker behind a typed
interface so downstream tooling (eligibility precheck, agent
dispatch, status reporters) doesn't reach into BACKLOG.md table
syntax directly. This module is the fork's port of that pattern.

The implementation is **read-only** by design — mutations to
BACKLOG.md remain a manual editorial task per CLAUDE.md global rule
"Read AND update local state files".

Row format reference. The current checklist schema puts a stable ID in a
backtick span immediately after the checkbox::

    2. [ ] `T-RC1-MASTER-GREEN` **Master green** (#1236)
    7. [ ] `T-RC2-BENCH-TUNE` **[BLOCKED]** **Bench and tune**
    1. [x] `T-RC1-RELEASE-PIPELINE` **Release pipeline correct**

Checked items are ``DONE``. Unchecked items are ``OPEN`` unless the title
starts with ``**[BLOCKED]**``, ``**[DEFERRED]**``, or
``**[IN_FLIGHT]**``. Continuation lines belong to the preceding item, so PR
references remain discoverable after Markdown wrapping.

The retired table schema remains readable during migration::

    | **T3-7** | open row title | ... |
        ^^^^^^ — open: ID wrapped in bold only

    | ~~**T0-1**~~ **[DONE — PR #72 (Batch-A, ADR-0131)]** | ... |
       ^^^^^^^^^^                  ^^
        strike-through ID         status marker

    | ~~**T2-1**~~ | ~~text~~ | ~~S~~ | **DONE 2026-04-20** ... |
        — alternate phrasing: status word in a later cell

Status detection logic (apply in order):

    1. Row contains `[REMOVED` -> status=REMOVED
    2. Row contains `[CLOSED`  -> status=CLOSED
    3. Row contains `[DONE`    -> status=DONE
    4. Row contains `~~**T...**~~` (strikethrough ID) AND
       `**DONE` somewhere on the row    -> status=DONE
    5. Row contains `BLOCKED`           -> status=BLOCKED
    6. Row contains `DEFERRED`          -> status=DEFERRED
    7. Row contains `IN FLIGHT` or `IN_FLIGHT` -> status=IN_FLIGHT
    8. otherwise                        -> status=OPEN

PR refs are extracted via the regex `#(\\d+)` from any `[DONE — PR #N
...]` or `**DONE 2026-MM-DD via PR #N...** ` substring on the row.
"""

from __future__ import annotations

import json
import os
import re
import sys
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, cast

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.lib.safe_subprocess import run as run_command

# Number of `|`-separated cells before the title cell in a parsed
# BACKLOG.md row: cells[0] is the leading empty string before the
# first pipe, cells[1] holds the ID, cells[2] is the title.
_TITLE_CELL_INDEX = 2


class BacklogFormatError(ValueError):
    """The backlog exists but does not satisfy a supported schema."""


# ---------------------------------------------------------------------------
# Locating BACKLOG.md
# ---------------------------------------------------------------------------


# `.workingdir/` is git-ignored and lives in the **main** working
# tree, not inside per-agent worktrees. Walk parents until we find a
# directory that contains `.workingdir/BACKLOG.md`; if none exists,
# fall back to `<repo-root>/.workingdir/BACKLOG.md` (returns a path
# even if the file is absent so callers can detect that explicitly).
def _find_backlog(start: Path | None = None) -> Path:
    here = (start or Path(__file__).resolve()).parent
    for candidate in [here, *here.parents]:
        target = candidate / ".workingdir" / "BACKLOG.md"
        if target.is_file():
            return target
        # Worktree case: the per-agent tree lives under
        # `<main-repo>/.claude/worktrees/<id>/`. The main repo's
        # `.workingdir/` is `<main-repo>/.workingdir/`, i.e. up
        # three directories from here.
        if candidate.name.startswith("agent-") and candidate.parent.name == "worktrees":
            main_repo = candidate.parent.parent.parent
            target2 = main_repo / ".workingdir" / "BACKLOG.md"
            if target2.is_file():
                return target2
    # Last-ditch fallback: assume the script is being run from the
    # repo root and BACKLOG.md is sibling to scripts/.
    return Path.cwd() / ".workingdir" / "BACKLOG.md"


DEFAULT_BACKLOG_PATH: Path = _find_backlog()


# ---------------------------------------------------------------------------
# Typed BacklogItem
# ---------------------------------------------------------------------------


@dataclass
class BacklogItem:
    """One tracked BACKLOG.md item, parsed into a typed shape."""

    id: str
    """Stable ID, e.g. ``T-RC1-MASTER-GREEN`` or legacy ``T3-9``."""

    title: str
    """Display title from the item start, with markdown stripped."""

    status: str
    """One of OPEN / IN_FLIGHT / DONE / CLOSED / REMOVED / BLOCKED / DEFERRED."""

    priority: int | None
    """Tier number parsed from the prefix: ``T0`` → 0, ``T7`` → 7,
    ``TA-`` (addendum / cross-stream) → ``None``."""

    pr_refs: list[int] = field(default_factory=list)
    """PR numbers parsed from any ``PR #N`` markers on the row."""

    raw_row: str = ""
    """Original markdown row, useful for diagnostics."""

    def is_closed(self) -> bool:
        return self.status in {"DONE", "CLOSED", "REMOVED"}


# ---------------------------------------------------------------------------
# BacklogTracker
# ---------------------------------------------------------------------------


# Stable ID forms we recognise:
#   **T3-9**             — open
#   ~~**T0-1**~~         — strike-through ID (closed forms)
#   **TA-VOCAB**         — addendum row
#   **T7-10b**           — sub-row with a letter suffix
_ITEM_ID = r"T[0-9A-Za-z]*(?:-[0-9A-Za-z]+)+"
_ID_PATTERN = re.compile(
    rf"""
    ^\|\s*                          # leading pipe + optional whitespace
    (?:~~)?                         # optional strike-through opener
    \*\*                            # bold opener
    (?P<id>
        {_ITEM_ID}                     # tiered or named stable ID
    )
    \*\*                            # bold closer
    (?:~~)?                         # optional strike-through closer
    # The first cell may contain trailing markup after the ID — e.g.
    # `~~**T0-1**~~ **[DONE — PR #72 (ADR-0131)]**` — before the
    # next pipe. Consume anything that isn't a pipe.
    [^|]*
    \|                              # close of the first cell
    """,
    re.VERBOSE,
)

_CHECKLIST_PATTERN = re.compile(
    rf"^\s*(?:[-+*]|\d+[.)])\s+\[(?P<checked>[ xX])\]\s+"
    rf"`(?P<id>{_ITEM_ID})`(?:\s+|$)(?P<title>.*)$"
)
_ANY_CHECKLIST_PATTERN = re.compile(r"^\s*(?:[-+*]|\d+[.)])\s+\[[ xX]\]\s+")
_CHECKLIST_STATUS_PATTERN = re.compile(
    r"^\*\*\[(?P<status>[A-Z][A-Z _-]*)\]\*\*\s*",
    re.IGNORECASE,
)
_CHECKLIST_OPEN_STATUSES = {
    "BLOCKED": "BLOCKED",
    "DEFERRED": "DEFERRED",
    "IN FLIGHT": "IN_FLIGHT",
}
_CHECKLIST_CLOSED_STATUSES = {"DONE", "CLOSED", "REMOVED"}

# Match `PR #123` (case-insensitive, optional space). Not anchored.
_PR_REF_PATTERN = re.compile(r"PR\s*#(\d+)", re.IGNORECASE)

# Status precedence (first match wins).
_STATUS_RULES: list[tuple[re.Pattern[str], str]] = [
    (re.compile(r"\[REMOVED", re.IGNORECASE), "REMOVED"),
    (re.compile(r"\[CLOSED", re.IGNORECASE), "CLOSED"),
    (re.compile(r"\[DONE", re.IGNORECASE), "DONE"),
    # Strike-through ID + a "DONE" word elsewhere on the row.
    (re.compile(r"~~\*\*T[0-9A-Z]+(?:-[0-9A-Za-z]+)?\*\*~~"), "DONE"),
    (re.compile(r"\*\*DONE\b", re.IGNORECASE), "DONE"),
    (re.compile(r"\bBLOCKED\b", re.IGNORECASE), "BLOCKED"),
    (re.compile(r"\bDEFERRED\b", re.IGNORECASE), "DEFERRED"),
    (re.compile(r"\bIN[ _-]FLIGHT\b", re.IGNORECASE), "IN_FLIGHT"),
]


def _extract_priority(item_id: str) -> int | None:
    """Parse the tier number from the ID prefix.

    ``T3-9``    → 3
    ``T7-10b``  → 7
    ``TA-VOCAB`` → None  (addendum rows aren't tiered)
    """
    m = re.match(r"T(\d+)", item_id)
    return int(m.group(1)) if m else None


def _strip_md(cell: str) -> str:
    """Remove markdown emphasis / links / strikethroughs from a cell."""
    cell = re.sub(r"~~", "", cell)
    cell = re.sub(r"\*\*", "", cell)
    cell = re.sub(r"\*", "", cell)
    cell = re.sub(r"`", "", cell)
    # Drop link targets but keep the link text: `[text](url)` -> `text`
    cell = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", cell)
    return cell.strip()


def _classify_status(row: str) -> str:
    for pattern, status in _STATUS_RULES:
        if pattern.search(row):
            return status
    return "OPEN"


def _parse_table_item(raw: str) -> BacklogItem | None:
    match = _ID_PATTERN.match(raw)
    if not match:
        return None
    item_id = match.group("id")
    cells = raw.split("|")
    title = _strip_md(cells[_TITLE_CELL_INDEX]) if len(cells) > _TITLE_CELL_INDEX else ""
    return BacklogItem(
        id=item_id,
        title=title,
        status=_classify_status(raw),
        priority=_extract_priority(item_id),
        pr_refs=sorted({int(ref) for ref in _PR_REF_PATTERN.findall(raw)}),
        raw_row=raw,
    )


def _checklist_status(checked: str, title: str, line_number: int) -> tuple[str, str]:
    marker = _CHECKLIST_STATUS_PATTERN.match(title)
    explicit = re.sub(r"[ _-]+", " ", marker.group("status").upper()).strip() if marker else ""
    clean_title = title[marker.end() :] if marker else title
    known_statuses = {*_CHECKLIST_OPEN_STATUSES, *_CHECKLIST_CLOSED_STATUSES}
    if explicit and explicit not in known_statuses:
        raise BacklogFormatError(
            f"checklist item on line {line_number} has unsupported status {explicit}"
        )
    if checked.lower() == "x":
        if explicit:
            raise BacklogFormatError(
                f"checked checklist item on line {line_number} conflicts with status {explicit}"
            )
        return "DONE", clean_title
    if explicit in _CHECKLIST_CLOSED_STATUSES:
        raise BacklogFormatError(
            f"unchecked checklist item on line {line_number} "
            f"conflicts with closed status {explicit}"
        )
    return _CHECKLIST_OPEN_STATUSES.get(explicit, "OPEN"), clean_title


def _finish_checklist_item(
    start_line: int,
    match: re.Match[str],
    lines: list[str],
) -> tuple[int, BacklogItem]:
    raw = "\n".join(lines).rstrip()
    status, title = _checklist_status(match.group("checked"), match.group("title"), start_line)
    item_id = match.group("id")
    return start_line, BacklogItem(
        id=item_id,
        title=_strip_md(title),
        status=status,
        priority=_extract_priority(item_id),
        pr_refs=sorted({int(ref) for ref in _PR_REF_PATTERN.findall(raw)}),
        raw_row=raw,
    )


def _parse_checklist_items(lines: list[str]) -> list[tuple[int, BacklogItem]]:
    items: list[tuple[int, BacklogItem]] = []
    current: tuple[int, re.Match[str], list[str]] | None = None
    for line_number, raw in enumerate(lines, start=1):
        match = _CHECKLIST_PATTERN.match(raw)
        if match:
            if current:
                items.append(_finish_checklist_item(*current))
            current = (line_number, match, [raw])
            continue
        if _ANY_CHECKLIST_PATTERN.match(raw):
            raise BacklogFormatError(
                f"checklist item on line {line_number} needs a stable backtick ID after its checkbox"
            )
        if current and (not raw.strip() or raw[:1].isspace()):
            current[2].append(raw)
        elif current:
            items.append(_finish_checklist_item(*current))
            current = None
    if current:
        items.append(_finish_checklist_item(*current))
    return items


def _reject_duplicate_ids(items: list[BacklogItem]) -> None:
    seen: set[str] = set()
    for item in items:
        if item.id in seen:
            raise BacklogFormatError(f"duplicate backlog ID {item.id}")
        seen.add(item.id)


class BacklogTracker:
    """Parse `.workingdir/BACKLOG.md` into typed `BacklogItem` rows.

    Lazy: parses on first call to :meth:`_rows` and caches the
    result. Re-construct the tracker to pick up edits.
    """

    def __init__(self, path: Path | os.PathLike[str] | None = None) -> None:
        self.path: Path = Path(path) if path is not None else DEFAULT_BACKLOG_PATH
        self._cached: list[BacklogItem] | None = None

    # -- Internals ---------------------------------------------------

    def _rows(self) -> list[BacklogItem]:
        if self._cached is not None:
            return self._cached
        if not self.path.is_file():
            self._cached = []
            return self._cached

        text = self.path.read_text(encoding="utf-8")
        lines = text.splitlines()
        indexed_items = _parse_checklist_items(lines)
        for line_number, raw in enumerate(lines, start=1):
            item = _parse_table_item(raw)
            if item:
                indexed_items.append((line_number, item))
        indexed_items.sort(key=lambda entry: entry[0])
        items = [item for _, item in indexed_items]
        if not items:
            raise BacklogFormatError(f"{self.path} contains no tracked items")
        _reject_duplicate_ids(items)
        self._cached = items
        return self._cached

    # -- Public API --------------------------------------------------

    def all(self) -> list[BacklogItem]:
        """Return every parsed row, in source order."""
        return list(self._rows())

    def get(self, item_id: str) -> BacklogItem | None:
        """Look up a single item by ID. Returns None if absent."""
        for it in self._rows():
            if it.id == item_id:
                return it
        return None

    def list_open(self) -> list[BacklogItem]:
        """All rows whose status is OPEN."""
        return [it for it in self._rows() if it.status == "OPEN"]

    def list_in_flight(self) -> list[BacklogItem]:
        """Rows tagged IN_FLIGHT (rare — usually status lives in PRs)."""
        return [it for it in self._rows() if it.status == "IN_FLIGHT"]

    def list_closed(self) -> list[BacklogItem]:
        """Rows whose status is DONE / CLOSED / REMOVED."""
        return [it for it in self._rows() if it.is_closed()]


# ---------------------------------------------------------------------------
# GitHubTracker
# ---------------------------------------------------------------------------


@dataclass
class _PR:
    """Lightweight, dict-compatible PR shape returned by ``gh pr list``."""

    number: int
    title: str
    body: str
    head_ref: str
    merged_at: str | None


def _decode_object_list(raw: str) -> list[dict[str, Any]]:
    """Decode a JSON array whose entries are objects with string keys."""
    payload: Any = json.loads(raw)
    if not isinstance(payload, list):
        raise ValueError("GitHub response must be a JSON array")
    objects: list[dict[str, Any]] = []
    for index, entry in enumerate(payload):
        if not isinstance(entry, dict) or not all(isinstance(key, str) for key in entry):
            raise ValueError(f"GitHub response entry {index} must be an object")
        objects.append(cast(dict[str, Any], entry))
    return objects


class GitHubTracker:
    """Wrap `gh` CLI for PR / branch lookups used by the precheck.

    The class is intentionally thin: it shells out to `gh` and parses
    its JSON. No caching, no auth handling — that's `gh`'s job. The
    tests stub `_run` to avoid network calls.
    """

    def __init__(self, repo: str = "VMAFx/vmafx") -> None:
        self.repo = repo

    # -- Internals ---------------------------------------------------

    def _run(self, *args: str) -> str:
        """Run ``gh`` and return stdout. Raises on non-zero exit.

        ``gh`` is the only command this wrapper ever executes. Resolve it to
        an absolute path before appending the fixed API argument vectors.
        """
        cmd = ["gh", *args]
        result = run_command(
            cmd,
            allowed_executables=("gh",),
            check=True,
            capture_output=True,
            text=True,
            timeout_seconds=60,
        )
        return result.stdout

    # -- Public API --------------------------------------------------

    def merged_prs_since(self, ts: datetime) -> list[dict[str, Any]]:
        """Return merged PRs whose ``mergedAt >= ts``.

        Uses ``gh pr list --state merged`` with a search filter. The
        return value is a list of dicts (not :class:`_PR`) to match
        the user's requested signature.
        """
        iso = ts.astimezone(timezone.utc).strftime("%Y-%m-%d")
        out = self._run(
            "pr",
            "list",
            "--repo",
            self.repo,
            "--state",
            "merged",
            "--search",
            f"merged:>={iso}",
            "--json",
            "number,title,body,headRefName,mergedAt",
            "--limit",
            "200",
        )
        return _decode_object_list(out)

    def search_prs(self, query: str, state: str = "all", limit: int = 30) -> list[dict[str, Any]]:
        """Free-form ``gh pr list --search`` wrapper.

        Returns a list of dicts with ``number / title / body /
        headRefName / state / mergedAt``. Used by the eligibility
        precheck to find PRs whose title or body mentions a backlog ID.
        """
        out = self._run(
            "pr",
            "list",
            "--repo",
            self.repo,
            "--state",
            state,
            "--search",
            query,
            "--json",
            "number,title,body,headRefName,state,mergedAt",
            "--limit",
            str(limit),
        )
        return _decode_object_list(out)

    def open_agent_branches(self) -> list[str]:
        """List head-branch names of open PRs that look like agent runs.

        "Agent-shape" branches start with ``agent-``, ``feat-``,
        ``feat/``, ``chore-``, or ``chore/``. Returned in
        creation-newest-first order from ``gh pr list``.
        """
        out = self._run(
            "pr",
            "list",
            "--repo",
            self.repo,
            "--state",
            "open",
            "--json",
            "headRefName",
            "--limit",
            "100",
        )
        rows = json.loads(out)
        prefixes = ("agent-", "agent/", "feat-", "feat/", "chore-", "chore/")
        return [
            row["headRefName"] for row in rows if row.get("headRefName", "").startswith(prefixes)
        ]


# ---------------------------------------------------------------------------
# Convenience: combined sanity helper
# ---------------------------------------------------------------------------


def explain(item_id: str, *, backlog: BacklogTracker | None = None) -> str:
    """Human-readable one-line summary of an item's tracker state.

    Used by the eligibility precheck error messages.
    """
    bk = backlog or BacklogTracker()
    item = bk.get(item_id)
    if item is None:
        return f"{item_id}: not found in BACKLOG.md"
    pr_str = ", ".join(f"#{n}" for n in item.pr_refs) or "no PR refs"
    return f"{item.id}: status={item.status} priority={item.priority} prs=[{pr_str}]"


__all__ = [
    "DEFAULT_BACKLOG_PATH",
    "BacklogFormatError",
    "BacklogItem",
    "BacklogTracker",
    "GitHubTracker",
    "explain",
]
