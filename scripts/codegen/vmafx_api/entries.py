# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Readers shared by every table of the definition: keys, versions, deprecations."""

from __future__ import annotations

from typing import Any

from .model import SINCE_PARTS, VERSION_PARTS, DefinitionError, Deprecation

Entry = dict[str, Any]


def need(entry: Entry, key: str, where: str) -> Any:
    if key not in entry:
        raise DefinitionError(f"{where}: missing `{key}`")
    return entry[key]


def where_of(table: str, entry: Entry) -> str:
    return f"{table}[{entry.get('name', entry.get('path', '?'))}]"


def version(text: object, where: str, parts: int = SINCE_PARTS) -> tuple[int, ...]:
    pieces = str(text).split(".")
    if len(pieces) != parts or not all(p.isdigit() for p in pieces):
        shape = "MAJOR.MINOR.PATCH" if parts == VERSION_PARTS else "MAJOR.MINOR"
        raise DefinitionError(f"{where}: version must be {shape}, got {text!r}")
    return tuple(int(p) for p in pieces)


def since_of(entry: Entry, where: str, inherited: tuple[int, int] | None = None) -> tuple[int, int]:
    """`since` of an entry; members (fields, values, bits) inherit their parent's."""
    if "since" not in entry and inherited is not None:
        return inherited
    major, minor = version(need(entry, "since", where), where)
    return major, minor


def deprecation(entry: Entry, where: str) -> Deprecation | None:
    raw = entry.get("deprecated")
    if raw is None:
        return None
    if not isinstance(raw, dict):
        raise DefinitionError(f"{where}: `deprecated` is {{since, replacement, removal}}")
    where = f"{where}.deprecated"
    since = version(need(raw, "since", where), where)
    removal = version(need(raw, "removal", where), where)
    replacement = str(need(raw, "replacement", where)).strip()
    if not replacement:
        raise DefinitionError(f"{where}: `replacement` names what to use instead")
    return Deprecation(
        since=(since[0], since[1]), replacement=replacement, removal=(removal[0], removal[1])
    )
