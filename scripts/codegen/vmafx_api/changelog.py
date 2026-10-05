# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Changelog draft from two definitions (`vmafx-api.py --changelog <base-ref>`).

Prints the text of `changelog.d/added/api-<slug>.md` (symbols, types, fields,
constants and options the definition gained) and `changelog.d/changed/api-<slug>.md`
(entries newly deprecated) on stdout. The author picks the slug, edits the
wording and commits the fragments (ADR-0221); nothing is written here.
"""

from __future__ import annotations

from .abi_check import report
from .model import Api, Deprecation, version_text
from .validate import entries, members


def _deprecations(api: Api) -> dict[str, Deprecation]:
    out = {name: dep for _, name, _, dep in entries(api) if dep is not None}
    out.update({name: dep for name, _, _, dep in members(api) if dep is not None})
    for group in api.option_groups:
        for option in group.options:
            if option.deprecated is not None:
                out[f"{group.name}.{option.name}"] = option.deprecated
    return out


def _bullet(title: str, items: list[str]) -> str:
    listed = ", ".join(f"`{item}`" for item in items)
    return f"- **{title}** {listed}.\n"


def _added_text(old: Api, new: Api) -> str:
    added = report(old, new).added
    if not added:
        return ""
    groups: dict[str, list[str]] = {}
    for text in added:
        kind, _, name = text.partition(" ")
        groups.setdefault(kind, []).append(name)
    lines = [
        f"- **VMAFx API {version_text(new.abi_minor_node)} additions** ([reference](docs/api/vmafx/reference.md)):\n"
    ]
    lines += [
        f"  - {kind}: " + ", ".join(f"`{n}`" for n in names) + "\n"
        for kind, names in groups.items()
    ]
    return "".join(lines)


def _changed_text(old: Api, new: Api) -> str:
    previous = _deprecations(old)
    fresh = {name: dep for name, dep in _deprecations(new).items() if name not in previous}
    lines = []
    for name, dep in sorted(fresh.items()):
        since, removal = version_text(dep.since), version_text(dep.removal)
        lines.append(
            f"- **`{name}` is deprecated** since VMAFx API {since}: use `{dep.replacement}`; it is removed in {removal}.\n"
        )
    return "".join(lines)


def draft(old: Api, new: Api) -> str:
    """Both fragments with their target paths; a note when there is nothing to record."""
    sections = []
    for kind, text in (("added", _added_text(old, new)), ("changed", _changed_text(old, new))):
        if text:
            sections.append(f"--- changelog.d/{kind}/api-<slug>.md\n{text}")
    if not sections:
        return "no API additions or deprecations against the base definition\n"
    return "\n".join(sections)


__all__ = ["draft"]
