# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Load, validate and render the credits list (ADR-2485).

``docs/credits.yaml`` is the curated list of third-party items VMAFx ships,
vendors, adapts, learns from or uses. ``docs/credits.md`` holds hand-written
prose and, between ``<!-- credits:table ID -->`` and ``<!-- credits:end -->``
markers, tables this module renders from the list. ``generate-credits.py``
writes and checks the tables; ``check-credits.py`` holds the list to the
repository. Both call the functions here, so there is one implementation.
"""

from __future__ import annotations

import datetime
import os
import re
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import yaml  # type: ignore[import-untyped]  # PyYAML ships no stubs in the hook env, as in scripts/ci/check_composite_actions.py

from scripts.lib.safe_subprocess import run as run_command

KINDS = (
    "upstream",
    "code",
    "library",
    "tool",
    "action",
    "image",
    "font",
    "model",
    "dataset",
    "paper",
    "standard",
    "text",
)
RELATIONS = ("shipped", "vendored", "adapted", "inspired", "used-by-CI", "integrated")
LICENSE_WORDS = ("proprietary", "none", "unknown")
REQUIRED = ("id", "name", "url", "kind", "relation", "license")
OPTIONAL = ("license_note", "paths", "evidence", "note")
ID_RE = re.compile(r"^[a-z0-9][a-z0-9.+-]*$")
SPDX_TOKEN_RE = re.compile(r"^(?:LicenseRef-[A-Za-z0-9.-]+|[A-Za-z0-9][A-Za-z0-9.+-]*)$")
SPDX_OPERATORS = ("AND", "OR", "WITH")
# GNU licences the SPDX list deprecates without a suffix.
DEPRECATED = ("GPL-2.0", "GPL-3.0", "LGPL-2.0", "LGPL-2.1", "LGPL-3.0", "AGPL-3.0", "GFDL-1.3")
# Licences of the project's own code: their texts under LICENSES/ need no entry.
OWN_LICENSES = ("EUPL-1.2", "BSD-2-Clause-Patent")

# Page sections: id -> (heading, kinds). The order is the order on the page.
SECTIONS = (
    ("code", "Upstream projects and vendored or adapted code", ("upstream", "code")),
    ("libraries", "Libraries", ("library",)),
    ("models", "Models", ("model",)),
    ("datasets", "Datasets", ("dataset",)),
    ("papers", "Papers and standards", ("paper", "standard")),
    ("texts", "Adapted texts and inspirations", ("text",)),
    ("tools", "Build and CI tools, actions and images", ("tool", "action", "image")),
    ("fonts", "Fonts", ("font",)),
)
TABLE_BEGIN = "<!-- credits:table {id} -->"
TABLE_END = "<!-- credits:end -->"
MARKER_RE = re.compile(
    r"<!-- credits:table (?P<id>[a-z-]+) -->\n.*?<!-- credits:end -->", re.DOTALL
)
MAX_PATHS_SHOWN = 4
GIT_BYTES = 64 * 1_048_576


class CreditsError(Exception):
    """The credits list or page is malformed; the message names the entry."""


@dataclass(frozen=True)
class Entry:
    id: str
    name: str
    url: str
    kind: str
    relation: str
    license: str
    license_note: str = ""
    paths: tuple[str, ...] = ()
    evidence: tuple[str, ...] = ()
    note: str = ""


@dataclass(frozen=True)
class Waiver:
    """One file excused from one rule until a date (exceptions name file, rule, reason, expiry)."""

    path: str
    rule: str
    reason: str
    expires: datetime.date


def _text(raw: dict[str, Any], key: str, where: str) -> str:
    value = raw.get(key, "")
    if not isinstance(value, str):
        raise CreditsError(f"{where}: '{key}' must be a string")
    return value.strip()


def _strings(raw: dict[str, Any], key: str, where: str) -> tuple[str, ...]:
    value = raw.get(key, [])
    if not isinstance(value, list) or not all(isinstance(v, str) and v for v in value):
        raise CreditsError(f"{where}: '{key}' must be a list of non-empty strings")
    return tuple(value)


def license_tokens(expression: str) -> list[str]:
    """Return the licence identifiers of an SPDX expression, operators dropped."""
    cleaned = expression.replace("(", " ").replace(")", " ")
    return [t for t in cleaned.split() if t not in SPDX_OPERATORS]


def _check_license(expression: str, where: str) -> None:
    if expression in LICENSE_WORDS:
        return
    tokens = license_tokens(expression)
    if not tokens:
        raise CreditsError(f"{where}: empty licence")
    for token in tokens:
        if not SPDX_TOKEN_RE.match(token):
            raise CreditsError(f"{where}: '{token}' is not an SPDX identifier")
        if token.rstrip("+") in DEPRECATED:
            raise CreditsError(
                f"{where}: '{token}' is deprecated; write -only or -or-later as upstream states it"
            )


def _entry(raw: object, index: int) -> Entry:
    if not isinstance(raw, dict):
        raise CreditsError(f"entry {index}: not a mapping")
    where = f"entry {raw.get('id', index)}"
    unknown = sorted(set(raw) - set(REQUIRED) - set(OPTIONAL))
    if unknown:
        raise CreditsError(f"{where}: unknown keys {unknown}")
    values = {key: _text(raw, key, where) for key in (*REQUIRED, "license_note", "note")}
    missing = [key for key in REQUIRED if not values[key]]
    if missing:
        raise CreditsError(f"{where}: missing {missing}")
    if not ID_RE.match(values["id"]):
        raise CreditsError(f"{where}: id must be lower-case kebab")
    if not values["url"].startswith("https://"):
        raise CreditsError(f"{where}: url must be https")
    if values["kind"] not in KINDS:
        raise CreditsError(f"{where}: kind '{values['kind']}' not in {KINDS}")
    if values["relation"] not in RELATIONS:
        raise CreditsError(f"{where}: relation '{values['relation']}' not in {RELATIONS}")
    _check_license(values["license"], where)
    return Entry(
        **values,
        paths=_strings(raw, "paths", where),
        evidence=_strings(raw, "evidence", where),
    )


def load_entries(path: Path) -> list[Entry]:
    """Read and validate the credits list; ids are unique."""
    try:
        data = yaml.safe_load(path.read_text(encoding="utf-8"))
    except (OSError, yaml.YAMLError) as err:
        raise CreditsError(f"{path}: cannot read: {err}") from err
    if not isinstance(data, dict) or not isinstance(data.get("entries"), list):
        raise CreditsError(f"{path}: expected a mapping with an 'entries' list")
    entries = [_entry(raw, i) for i, raw in enumerate(data["entries"])]
    dupes = [i for i, n in Counter(e.id for e in entries).items() if n > 1]
    if dupes:
        raise CreditsError(f"{path}: duplicate ids {dupes}")
    return entries


WAIVER_KEYS = ("path", "rule", "reason", "expires")
WAIVER_RULES = ("uncredited-path",)


def _waiver(raw: object, index: int) -> Waiver:
    where = f"exception {index}"
    if not isinstance(raw, dict) or set(raw) != set(WAIVER_KEYS):
        raise CreditsError(f"{where}: needs exactly {WAIVER_KEYS}")
    if raw["rule"] not in WAIVER_RULES:
        raise CreditsError(f"{where}: rule must be one of {WAIVER_RULES}")
    try:
        expires = datetime.date.fromisoformat(str(raw["expires"]))
    except ValueError as err:
        raise CreditsError(f"{where}: expires must be an ISO date") from err
    if not isinstance(raw["path"], str) or not isinstance(raw["reason"], str) or not raw["reason"]:
        raise CreditsError(f"{where}: path and reason must be non-empty strings")
    return Waiver(raw["path"], raw["rule"], raw["reason"], expires)


def load_waivers(path: Path) -> list[Waiver]:
    """The ``exceptions`` list of the credits file; each one expires."""
    try:
        data = yaml.safe_load(path.read_text(encoding="utf-8"))
    except (OSError, yaml.YAMLError) as err:
        raise CreditsError(f"{path}: cannot read: {err}") from err
    raw = data.get("exceptions", []) if isinstance(data, dict) else []
    if not isinstance(raw, list):
        raise CreditsError(f"{path}: 'exceptions' must be a list")
    return [_waiver(item, i) for i, item in enumerate(raw)]


def _cell(text: str) -> str:
    return text.replace("|", "\\|").replace("\n", " ")


def _paths_cell(entry: Entry) -> str:
    shown = [f"`{p}`" for p in entry.paths[:MAX_PATHS_SHOWN]]
    extra = len(entry.paths) - MAX_PATHS_SHOWN
    if extra > 0:
        shown.append(f"and {extra} more")
    return ", ".join(shown) if shown else "see the note"


def _license_cell(entry: Entry) -> str:
    base = f"`{entry.license}`"
    return f"{base} ({_cell(entry.license_note)})" if entry.license_note else base


def render_table(entries: list[Entry]) -> str:
    """One Markdown table, rows sorted by name."""
    rows = [
        "| Item | Relation | Licence | Where it is used | Note |",
        "| --- | --- | --- | --- | --- |",
    ]
    for e in sorted(entries, key=lambda x: x.name.lower()):
        rows.append(
            f"| [{_cell(e.name)}]({e.url}) | {e.relation} | {_license_cell(e)} "
            f"| {_paths_cell(e)} | {_cell(e.note)} |"
        )
    return "\n".join(rows)


def render_unverified(entries: list[Entry]) -> str:
    unknown = sorted((e for e in entries if e.license == "unknown"), key=lambda x: x.name.lower())
    if not unknown:
        return "Every entry states a licence."
    names = ", ".join(f"[{_cell(e.name)}]({e.url})" for e in unknown)
    return f"{len(unknown)} entries have a licence this page could not verify upstream: {names}."


def render_counts(entries: list[Entry]) -> str:
    by_kind = Counter(e.kind for e in entries)
    by_rel = Counter(e.relation for e in entries)
    kinds = ", ".join(f"{k} {by_kind[k]}" for k in KINDS if by_kind[k])
    rels = ", ".join(f"{r} {by_rel[r]}" for r in RELATIONS if by_rel[r])
    return f"{len(entries)} entries. By kind: {kinds}. By relation: {rels}."


def blocks(entries: list[Entry]) -> dict[str, str]:
    """The generated text of every marker block, keyed by block id."""
    out = {sid: render_table([e for e in entries if e.kind in kinds]) for sid, _, kinds in SECTIONS}
    out["summary"] = render_counts(entries) + "\n\n" + render_unverified(entries)
    return out


def apply_blocks(page: str, generated: dict[str, str]) -> str:
    """Return ``page`` with every marker block replaced by its generated text."""
    seen: set[str] = set()

    def swap(match: re.Match[str]) -> str:
        block = match.group("id")
        if block not in generated:
            raise CreditsError(f"docs/credits.md: unknown block '{block}'")
        seen.add(block)
        return f"{TABLE_BEGIN.format(id=block)}\n{generated[block]}\n{TABLE_END}"

    result = MARKER_RE.sub(swap, page)
    missing = sorted(set(generated) - seen)
    if missing:
        raise CreditsError(f"docs/credits.md: no marker block for {missing}")
    return result


def tracked_files(root: Path) -> list[str]:
    """Files Git lists for the checkout: tracked, and untracked but not ignored."""
    env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
    result = run_command(
        ("git", "-C", str(root), "ls-files", "-z", "--cached", "--others", "--exclude-standard"),
        allowed_executables=("git",),
        env=env,
        capture_output=True,
        text=True,
        check=True,
        max_output_bytes=GIT_BYTES,
    )
    return sorted(n for n in result.stdout.split("\0") if n and (root / n).is_file())


def _glob_regex(pattern: str) -> re.Pattern[str]:
    out = []
    i = 0
    while i < len(pattern):
        if pattern.startswith("**", i):
            out.append(".*")
            i += 2
        elif pattern[i] == "*":
            out.append("[^/]*")
            i += 1
        else:
            out.append(re.escape(pattern[i]))
            i += 1
    return re.compile("".join(out) + r"\Z")


def path_matches(pattern: str, name: str) -> bool:
    """True when ``name`` is ``pattern``, lies under it as a directory, or matches it as a glob."""
    base = pattern.rstrip("/")
    if name == base or name.startswith(base + "/"):
        return True
    return "*" in pattern and bool(_glob_regex(pattern).match(name))


def covered(name: str, entries: list[Entry]) -> bool:
    return any(path_matches(p, name) for e in entries for p in e.paths)
