# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Option groups of the definition (design section 3.4, ADR-1852).

One group per concept (model, feature, backend, device, threads, ...). Each
option carries its type, default, range or enumeration, documentation and one
spelling per surface the group is declared on: `cli` (list of flags),
`ffmpeg` (option name, `aliases` for upstream names), `mcp` (argument name)
and `proto` (`{ field = N }`). WP8 emits the surfaces; this module reads and
checks the data, so a group that misses a spelling for one of its surfaces
never reaches an emitter.
"""

from __future__ import annotations

from .entries import Entry, deprecation, need, since_of, where_of
from .model import DefinitionError, Option, OptionGroup

SURFACES = ("cli", "ffmpeg", "mcp", "proto")
OPTION_TYPES = ("bool", "int", "uint", "float", "string", "enum")
NUMERIC = ("int", "uint", "float")
PROTO_RESERVED = (19000, 19999)  # field numbers protobuf reserves for itself
PROTO_MAX = 536_870_911  # 2^29 - 1


def _spellings(raw: Entry, surfaces: tuple[str, ...], where: str) -> dict[str, tuple[str, ...]]:
    out: dict[str, tuple[str, ...]] = {}
    for surface in surfaces:
        if surface == "proto":
            continue
        value = raw.get(surface)
        if value in (None, "", []):
            raise DefinitionError(f"{where}: no `{surface}` spelling for a {surface} surface")
        names = tuple(value) if isinstance(value, list) else (str(value),)
        if surface == "ffmpeg":
            names += tuple(raw.get("aliases", []))
        out[surface] = names
    return out


def _proto_field(raw: Entry, surfaces: tuple[str, ...], where: str) -> int:
    if "proto" not in surfaces:
        return 0
    spec = raw.get("proto")
    if not isinstance(spec, dict) or "field" not in spec:
        raise DefinitionError(f"{where}: no `proto = {{ field = N }}` for a proto surface")
    number = int(spec["field"])
    if not 1 <= number <= PROTO_MAX or PROTO_RESERVED[0] <= number <= PROTO_RESERVED[1]:
        raise DefinitionError(f"{where}: proto field {number} is not a usable field number")
    return number


def _range(raw: Entry, kind: str, where: str) -> tuple[float, float] | None:
    if "range" not in raw:
        return None
    low, high = raw["range"]
    if kind not in NUMERIC or low > high:
        raise DefinitionError(f"{where}: `range` is [min, max] on a numeric option")
    return float(low), float(high)


def _check_default(option: Option, where: str) -> None:
    value = option.default
    if option.type == "enum" and value not in option.values:
        raise DefinitionError(f"{where}: default {value!r} is not one of {list(option.values)}")
    if option.type == "bool" and not isinstance(value, bool):
        raise DefinitionError(f"{where}: default of a bool option is true or false")
    if option.type == "string" and not isinstance(value, str):
        raise DefinitionError(f"{where}: default of a string option is a string")
    if option.type in NUMERIC:
        if isinstance(value, bool) or not isinstance(value, int | float):
            raise DefinitionError(f"{where}: default of a {option.type} option is a number")
        low, high = option.range or (float("-inf"), float("inf"))
        if not low <= value <= high or (option.type == "uint" and value < 0):
            raise DefinitionError(f"{where}: default {value} is outside {option.range}")


def _option(raw: Entry, group: OptionGroup, where: str) -> Option:
    where = f"{where}.{raw.get('name', '?')}"
    kind = need(raw, "type", where)
    if kind not in OPTION_TYPES:
        raise DefinitionError(f"{where}: option type is one of {OPTION_TYPES}")
    values = tuple(raw.get("enum", []))
    if (kind == "enum") != bool(values):
        raise DefinitionError(f"{where}: `enum` lists the values of an enum option, and only there")
    option = Option(
        name=need(raw, "name", where),
        type=kind,
        default=need(raw, "default", where),
        doc=need(raw, "doc", where),
        since=since_of(raw, where, group.since),
        deprecated=deprecation(raw, where),
        range=_range(raw, kind, where),
        values=values,
        spellings=_spellings(raw, group.surfaces, where),
        proto_field=_proto_field(raw, group.surfaces, where),
    )
    _check_default(option, where)
    return option


def _group(raw: Entry) -> OptionGroup:
    where = where_of("option_groups", raw)
    surfaces = tuple(need(raw, "surfaces", where))
    unknown = sorted(set(surfaces) - set(SURFACES))
    if unknown or not surfaces:
        raise DefinitionError(f"{where}: surfaces are a non-empty subset of {SURFACES}")
    shell = OptionGroup(
        name=need(raw, "name", where),
        doc=need(raw, "doc", where),
        since=since_of(raw, where),
        surfaces=surfaces,
        options=(),
    )
    options = tuple(_option(o, shell, where) for o in need(raw, "options", where))
    if not options:
        raise DefinitionError(f"{where}: a group holds at least one option")
    return OptionGroup(shell.name, shell.doc, shell.since, shell.surfaces, options)


def _unique_spellings(groups: tuple[OptionGroup, ...]) -> None:
    seen: dict[tuple[str, str], str] = {}
    for group in groups:
        for option in group.options:
            owner = f"{group.name}.{option.name}"
            keys = [(s, n) for s, names in option.spellings.items() for n in names]
            if option.proto_field:
                keys.append((f"proto:{group.name}", str(option.proto_field)))
            for key in keys:
                if key in seen:
                    raise DefinitionError(
                        f"{owner}: {key[0]} spelling {key[1]!r} also used by {seen[key]}"
                    )
                seen[key] = owner


def option_groups(raw: list[Entry]) -> tuple[OptionGroup, ...]:
    groups = tuple(_group(entry) for entry in raw)
    names = [g.name for g in groups]
    if len(set(names)) != len(names):
        raise DefinitionError(f"duplicate option group in {names}")
    for group in groups:
        option_names = [o.name for o in group.options]
        if len(set(option_names)) != len(option_names):
            raise DefinitionError(f"option_groups[{group.name}]: duplicate option name")
    _unique_spellings(groups)
    for group in groups:
        for option in group.options:
            if "cli" in option.spellings and not all(
                s.startswith("--") for s in option.spellings["cli"]
            ):
                raise DefinitionError(f"{group.name}.{option.name}: cli spellings start with --")
    return groups
