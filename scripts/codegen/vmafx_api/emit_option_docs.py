# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Option tables in the user documentation (design section 3.5).

Each page keeps its prose; the table of options sits between two marker lines
and is rendered from the option groups (`splice.py`). Pages and regions:

| Page | Region | Rows |
| --- | --- | --- |
| docs/usage/cli.md | `cli options` | every command-line option |
| docs/usage/ffmpeg.md | `vmafx filter options` | every option of the `vmafx` filter |
| docs/mcp/tools.md | `mcp scoring arguments` | every argument of the scoring tools |
| docs/server/api-contract.md | `score options` | every ScoreOptions field |
"""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path

from .model import Api, Option, OptionGroup
from .options import all_options
from .splice import spliced

Row = Callable[[OptionGroup, Option], list[str] | None]


def _cell(text: object) -> str:
    """Markdown table cell text: pipes escaped, angle brackets as entities."""
    escaped = str(text).replace("|", "\\|").replace("\n", " ")
    return escaped.replace("<", "&lt;").replace(">", "&gt;")


def _code(text: object) -> str:
    return f"`{text}`" if text not in (None, "") else ""


def value_text(option: Option) -> str:
    if option.values:
        return " \\| ".join(f"`{v}`" for v in option.values)
    if option.choices:
        return " \\| ".join(f"`{c}`" for c in option.choices)
    bounds = ""
    if option.range:
        low, high = option.range
        bounds = f" {low:g}..{high:g}" if high is not None else f" >= {low:g}"
    return option.type + bounds


def default_text(option: Option, surface: str) -> str:
    if option.default_macro:
        return f"library default (`{option.default_macro}`)"
    value = option.default_on(surface)
    if value is None or value == []:
        return ""
    if isinstance(value, bool):
        return _code(str(value).lower())
    if isinstance(value, list):
        return _code("+".join(value))
    return _code(value)


def _doc(option: Option) -> str:
    text = option.doc
    if option.reserved:
        text += f" Reserved: {option.reserved}; only the default is accepted."
    return _cell(text)


def _cli_row(_group: OptionGroup, option: Option) -> list[str] | None:
    if option.cli is None:
        return None
    flags = list(option.cli.flags) + [flag for _, flag in option.cli.values]
    names = ", ".join(f"`{f}`" for f in flags)
    short = _code(f"-{option.cli.short}") if option.cli.short else ""
    return [names, short, value_text(option), default_text(option, "cli"), _doc(option)]


def _ffmpeg_row(_group: OptionGroup, option: Option) -> list[str] | None:
    if "ffmpeg" not in option.surfaces:
        return None
    names = option.spellings["ffmpeg"]
    aliases = ", ".join(f"`{a}`" for a in names[1:])
    return [
        _code(names[0]),
        aliases,
        value_text(option),
        default_text(option, "ffmpeg"),
        _doc(option),
    ]


def _mcp_row(group: OptionGroup, option: Option) -> list[str] | None:
    if "mcp" not in option.surfaces:
        return None
    required = "yes" if option.mcp_required else ""
    tools = ", ".join(f"`{t}`" for t in group.mcp_tools)
    kind = value_text(option) + (" (list)" if "mcp" in option.repeat else "")
    return [
        _code(option.spellings["mcp"][0]),
        kind,
        required,
        default_text(option, "mcp"),
        tools,
        _doc(option),
    ]


def _proto_row(group: OptionGroup, option: Option) -> list[str] | None:
    if not option.proto_field:
        return None
    kind = value_text(option) + (" (list)" if "proto" in option.repeat else "")
    return [
        _code(option.name),
        str(option.proto_field),
        _code(group.proto_message),
        kind,
        default_text(option, "proto"),
        _doc(option),
    ]


REGIONS: tuple[tuple[str, str, tuple[str, ...], Row], ...] = (
    (
        "docs/usage/cli.md",
        "cli options",
        ("Option", "Short", "Value", "Default", "Description"),
        _cli_row,
    ),
    (
        "docs/usage/ffmpeg.md",
        "vmafx filter options",
        ("Option", "Aliases", "Value", "Default", "Description"),
        _ffmpeg_row,
    ),
    (
        "docs/mcp/tools.md",
        "mcp scoring arguments",
        ("Argument", "Type", "Required", "Default", "Tools", "Description"),
        _mcp_row,
    ),
    (
        "docs/server/api-contract.md",
        "score options",
        ("Field", "Number", "Message", "Value", "Default when unset", "Description"),
        _proto_row,
    ),
)


def markers(region: str) -> tuple[str, str]:
    return (
        f"<!-- BEGIN GENERATED: vmafx-api {region} (scripts/codegen/vmafx-api.py) -->",
        f"<!-- END GENERATED: vmafx-api {region} -->",
    )


def _row(cells: list[str]) -> str:
    """A table row in markdownlint's compact style: an empty cell is `| |`."""
    return "|" + "|".join(f" {cell} " if cell else " " for cell in cells) + "|"


def table(api: Api, columns: tuple[str, ...], row: Row) -> list[str]:
    lines = ["", "| " + " | ".join(columns) + " |", "|" + " --- |" * len(columns)]
    for group, option in all_options(api.option_groups):
        cells = row(group, option)
        if cells is not None:
            lines.append(_row(cells))
    return [*lines, ""]


def pages(api: Api, root: Path) -> dict[str, str]:
    """Every page with a generated option table: path -> text."""
    out: dict[str, str] = {}
    for path, region, columns, row in REGIONS:
        begin, end = markers(region)
        out[path] = spliced(root, path, begin, end, table(api, columns, row))
    return out
