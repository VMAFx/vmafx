# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Option groups of the definition (design section 3.4, ADR-1852).

One group per concept (model, feature, backend, device, threads, ...). Each
option carries its type, default, range or enumeration, documentation and one
spelling per surface it is on: `cli` (list of flags, plus `cli_short`,
`cli_meta`, `cli_id`, `cli_values`), `ffmpeg` (option name, `aliases` for
upstream names), `mcp` (argument name) and `proto` (`{ field = N }`). The
emitters (emit_cli, emit_mcp, emit_proto, emit_openapi, emit_ffmpeg_options,
emit_option_docs) render every surface from these records, so a group that
misses a spelling for one of its surfaces never reaches an emitter.
"""

from __future__ import annotations

import re

from .entries import Entry, deprecation, need, since_of, where_of
from .model import CliSpelling, DefinitionError, McpArgv, Option, OptionGroup

SURFACES = ("cli", "ffmpeg", "mcp", "proto")
OPTION_TYPES = ("bool", "int", "uint", "float", "string", "enum", "flags")
NUMERIC = ("int", "uint", "float")
PROTO_RESERVED = (19000, 19999)  # field numbers protobuf reserves for itself
PROTO_MAX = 536_870_911  # 2^29 - 1
ARGV_STAGES = ("core", "extra", "none")
RANGE_WITH_MAX = 2  # `range = [min, max]`; `[min]` has no maximum
IDENT = re.compile(r"^[a-z][a-z0-9_]*$")
MACRO = re.compile(r"^[A-Z][A-Z0-9_]*$")


def _names(raw: Entry, surface: str) -> tuple[str, ...]:
    value = raw.get(surface)
    names: tuple[str, ...] = ()
    if value not in (None, "", []):
        names = tuple(value) if isinstance(value, list) else (str(value),)
    if not names and surface != "cli":
        return ()
    if surface == "ffmpeg":
        names += tuple(raw.get("aliases", []))
    if surface == "cli":
        names += tuple(dict(raw.get("cli_values", {})).values())
    return names


def _spellings(raw: Entry, surfaces: tuple[str, ...], where: str) -> dict[str, tuple[str, ...]]:
    out: dict[str, tuple[str, ...]] = {}
    for surface in surfaces:
        if surface == "proto":
            continue
        names = _names(raw, surface)
        if not names:
            raise DefinitionError(f"{where}: no `{surface}` spelling for a {surface} surface")
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


def _range(raw: Entry, kind: str, where: str) -> tuple[float, float | None] | None:
    if "range" not in raw:
        return None
    bounds = list(raw["range"])
    if kind not in NUMERIC or len(bounds) not in (1, RANGE_WITH_MAX):
        raise DefinitionError(f"{where}: `range` is [min] or [min, max] on a numeric option")
    low = float(bounds[0])
    high = float(bounds[1]) if len(bounds) == RANGE_WITH_MAX else None
    if high is not None and low > high:
        raise DefinitionError(f"{where}: `range` minimum is above its maximum")
    return low, high


def _in_range(option: Option, value: object, where: str) -> None:
    if isinstance(value, bool) or not isinstance(value, int | float):
        raise DefinitionError(f"{where}: default of a {option.type} option is a number")
    low, high = option.range or (float("-inf"), None)
    if value < low or (high is not None and value > high) or (option.type == "uint" and value < 0):
        raise DefinitionError(f"{where}: default {value} is outside {option.range}")
    if option.choices and value not in option.choices:
        raise DefinitionError(f"{where}: default {value} is not one of {list(option.choices)}")


def _check_value(option: Option, value: object, where: str) -> None:
    """A default (or a surface's default) is a valid value of the option."""
    if value is None:
        return
    if option.type == "enum" and value not in option.values:
        raise DefinitionError(f"{where}: default {value!r} is not one of {list(option.values)}")
    if option.type == "flags" and (
        not isinstance(value, list) or not set(value) <= set(option.values)
    ):
        raise DefinitionError(f"{where}: default of a flags option is a list of its values")
    if option.type == "bool" and not isinstance(value, bool):
        raise DefinitionError(f"{where}: default of a bool option is true or false")
    if option.type == "string" and not isinstance(value, str):
        raise DefinitionError(f"{where}: default of a string option is a string")
    if option.type in NUMERIC:
        _in_range(option, value, where)


def _values(raw: Entry, kind: str, where: str) -> tuple[str, ...]:
    values = tuple(str(v) for v in raw.get("enum", []))
    if (kind in ("enum", "flags")) != bool(values):
        raise DefinitionError(f"{where}: `enum` lists the values of an enum or flags option")
    if len(set(values)) != len(values):
        raise DefinitionError(f"{where}: duplicate value in `enum`")
    return values


def _choices(raw: Entry, kind: str, where: str) -> tuple[int, ...]:
    choices = tuple(raw.get("choices", []))
    if choices and kind not in ("int", "uint"):
        raise DefinitionError(f"{where}: `choices` limits an integer option")
    if not all(isinstance(c, int) and not isinstance(c, bool) for c in choices):
        raise DefinitionError(f"{where}: `choices` are integers")
    return tuple(int(c) for c in choices)


def _cli(
    raw: Entry, option_name: str, kind: str, values: tuple[str, ...], where: str
) -> CliSpelling:
    short = str(raw.get("cli_short", ""))
    if short and (len(short) != 1 or not short.isalpha()):
        raise DefinitionError(f"{where}: `cli_short` is one letter")
    switches = tuple((str(k), str(v)) for k, v in dict(raw.get("cli_values", {})).items())
    if switches and (kind != "enum" or not {k for k, _ in switches} <= set(values)):
        raise DefinitionError(f"{where}: `cli_values` maps values of an enum option to switches")
    flags = tuple(raw.get("cli", []))
    if not all(str(f).startswith("--") for f in (*flags, *(s for _, s in switches))):
        raise DefinitionError(f"{option_name}: cli spellings start with --")
    meta = "" if kind == "bool" or switches else str(raw.get("cli_meta", _default_meta(kind)))
    ident = str(raw.get("cli_id", f"'{short}'" if short else f"ARG_{option_name.upper()}"))
    return CliSpelling(flags=flags, short=short, meta=meta, ident=ident, values=switches)


def _default_meta(kind: str) -> str:
    return {"int": "$integer", "uint": "$unsigned", "float": "$number"}.get(kind, "$string")


def _argv(raw: Entry, option: Option, where: str) -> McpArgv | None:
    """The `vmaf` flag a server passes for an MCP or proto argument, if any."""
    stage = str(raw.get("argv", "extra"))
    if stage not in ARGV_STAGES:
        raise DefinitionError(f"{where}: `argv` is one of {ARGV_STAGES}")
    suffix = str(raw.get("model_suffix", ""))
    served = {"mcp", "proto"} & set(option.surfaces)
    if stage == "none" or not served or (option.cli is None and not suffix):
        return None
    if suffix:
        return McpArgv(stage=stage, flag="", form="suffix", suffix=suffix)
    cli = option.cli
    assert cli is not None
    flag = str(raw.get("argv_flag", cli.flags[0] if cli.flags else ""))
    listed = bool({"mcp", "proto"} & set(option.repeat))
    form = (
        "choice"
        if cli.values
        else "repeat" if listed else "switch" if option.type == "bool" else "value"
    )
    return McpArgv(stage=stage, flag=flag, form=form)


def _repeat(raw: Entry, surfaces: tuple[str, ...], where: str) -> tuple[str, ...]:
    """`repeat = true` repeats on every surface; a list names the surfaces."""
    value = raw.get("repeat", False)
    if isinstance(value, bool):
        return surfaces if value else ()
    named = tuple(str(v) for v in value)
    if not set(named) <= set(surfaces):
        raise DefinitionError(f"{where}: `repeat` names surfaces of the option")
    return named


def _default(raw: Entry, where: str) -> tuple[object, str]:
    macro = str(raw.get("default_macro", ""))
    if macro and ("default" in raw or not MACRO.match(macro)):
        raise DefinitionError(f"{where}: `default_macro` names a C macro and replaces `default`")
    return raw.get("default"), macro


def _base_option(raw: Entry, group: OptionGroup, where: str) -> Option:
    kind = need(raw, "type", where)
    if kind not in OPTION_TYPES:
        raise DefinitionError(f"{where}: option type is one of {OPTION_TYPES}")
    surfaces = tuple(raw.get("surfaces", group.surfaces))
    if not surfaces or not set(surfaces) <= set(group.surfaces):
        raise DefinitionError(f"{where}: option surfaces are a subset of {group.surfaces}")
    name = need(raw, "name", where)
    if not IDENT.match(name):
        raise DefinitionError(f"{where}: option names are lower_snake_case")
    default, macro = _default(raw, where)
    values = _values(raw, kind, where)
    return Option(
        name=name,
        type=kind,
        default=default,
        doc=need(raw, "doc", where),
        since=since_of(raw, where, group.since),
        deprecated=deprecation(raw, where),
        range=_range(raw, kind, where),
        values=values,
        spellings=_spellings(raw, surfaces, where),
        proto_field=_proto_field(raw, surfaces, where),
        surfaces=surfaces,
        choices=_choices(raw, kind, where),
        repeat=_repeat(raw, surfaces, where),
        default_macro=macro,
        surface_defaults=tuple(dict(raw.get("surface_defaults", {})).items()),
        cli=_cli(raw, name, kind, values, where) if "cli" in surfaces else None,
        mcp_required=bool(raw.get("mcp_required", False)),
        reserved=str(raw.get("reserved", "")),
    )


def _option(raw: Entry, group: OptionGroup, where: str) -> Option:
    where = f"{where}.{raw.get('name', '?')}"
    base = _base_option(raw, group, where)
    if base.repeat and base.type != "string":
        raise DefinitionError(f"{where}: only string options repeat")
    if base.mcp_required and "mcp" not in base.surfaces:
        raise DefinitionError(f"{where}: `mcp_required` needs the mcp surface")
    for surface, value in base.surface_defaults:
        if surface not in base.surfaces:
            raise DefinitionError(
                f"{where}: surface default for {surface}, not one of its surfaces"
            )
        _check_value(base, value, f"{where}.surface_defaults.{surface}")
    _check_value(base, base.default, where)
    return Option(**{**base.__dict__, "argv": _argv(raw, base, where)})


def _group(raw: Entry) -> OptionGroup:
    where = where_of("option_groups", raw)
    surfaces = tuple(need(raw, "surfaces", where))
    unknown = sorted(set(surfaces) - set(SURFACES))
    if unknown or not surfaces:
        raise DefinitionError(f"{where}: surfaces are a non-empty subset of {SURFACES}")
    tools = tuple(raw.get("mcp_tools", []))
    if ("mcp" in surfaces) != bool(tools):
        raise DefinitionError(f"{where}: `mcp_tools` names the MCP tools of an mcp group")
    shell = OptionGroup(
        name=need(raw, "name", where),
        doc=need(raw, "doc", where),
        since=since_of(raw, where),
        surfaces=surfaces,
        options=(),
        mcp_tools=tools,
        proto_message=str(raw.get("proto_message", "ScoreOptions")),
    )
    options = tuple(_option(o, shell, where) for o in need(raw, "options", where))
    if not options:
        raise DefinitionError(f"{where}: a group holds at least one option")
    return OptionGroup(**{**shell.__dict__, "options": options})


def _spelling_keys(group: OptionGroup, option: Option) -> list[tuple[str, str]]:
    keys = [(s, n) for s, names in option.spellings.items() for n in names]
    if option.cli is not None and option.cli.short:
        keys.append(("cli", f"-{option.cli.short}"))
    if option.proto_field:
        keys.append((f"proto:{group.proto_message}", str(option.proto_field)))
    return keys


def _unique_spellings(groups: tuple[OptionGroup, ...]) -> None:
    seen: dict[tuple[str, str], str] = {}
    for group in groups:
        for option in group.options:
            owner = f"{group.name}.{option.name}"
            for key in _spelling_keys(group, option):
                if key in seen:
                    raise DefinitionError(
                        f"{owner}: {key[0]} spelling {key[1]!r} also used by {seen[key]}"
                    )
                seen[key] = owner


def _unique_names(groups: tuple[OptionGroup, ...]) -> None:
    names = [g.name for g in groups]
    if len(set(names)) != len(names):
        raise DefinitionError(f"duplicate option group in {names}")
    options = [o.name for g in groups for o in g.options]
    duplicates = sorted({n for n in options if options.count(n) > 1})
    if duplicates:
        raise DefinitionError(f"option names are unique across groups: {duplicates}")


def option_groups(raw: list[Entry]) -> tuple[OptionGroup, ...]:
    groups = tuple(_group(entry) for entry in raw)
    _unique_names(groups)
    _unique_spellings(groups)
    return groups


def all_options(groups: tuple[OptionGroup, ...]) -> list[tuple[OptionGroup, Option]]:
    """Every option with its group, in definition order."""
    return [(group, option) for group in groups for option in group.options]
