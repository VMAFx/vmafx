# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Scoring options generated from the option groups of ``core/api/vmafx.toml``.

``options.gen.json`` next to this module is written by
``scripts/codegen/vmafx-api.py`` (RC4 WP8, ADR-1852); never edit it. It holds
the input schemas of the scoring tools (JSON Schema 2020-12), every option
with its bounds and defaults, the argument-vector spec and the flag of every
``vmaf`` option. The Go server (``cmd/vmafx-mcp``, through ``pkg/scoreopts``)
reads a byte-identical copy, so both servers serve the same schemas, accept
the same values and build the same ``vmaf`` argument vectors.
"""

from __future__ import annotations

import copy
import json
import math
from functools import cache
from pathlib import Path
from typing import Any

GENERATED = Path(__file__).with_name("options.gen.json")
MAX_LIST_LEN = 64  # a repeated argument longer than this is refused (HISS-02)


@cache
def document() -> dict[str, Any]:
    """The generated document, read once."""
    loaded: dict[str, Any] = json.loads(GENERATED.read_text(encoding="utf-8"))
    return loaded


def tool_schema(tool: str) -> dict[str, Any]:
    """A copy of the generated input schema of an MCP tool."""
    schema: dict[str, Any] = copy.deepcopy(document()["tools"][tool])
    return schema


def option(name: str) -> dict[str, Any]:
    """The generated record of an option, by option name."""
    for record in document()["options"]:
        if record["name"] == name:
            found: dict[str, Any] = record
            return found
    raise KeyError(f"no generated option {name!r}")


def allowed(name: str) -> set[Any]:
    """The values an enumerated option takes."""
    return set(option(name)["schema"].get("enum", []))


def library_default(name: str) -> str:
    """The value of the library default an option names (the default model)."""
    macro = option(name)["default_macro"]
    value: str = document()["library_defaults"][macro]
    return value


def mcp_default(name: str) -> Any:
    """The default the schema documents for an option on the MCP surface."""
    record = option(name)
    return (record.get("surface_defaults") or {}).get("mcp", record["default"])


def flag(name: str) -> str:
    """The ``vmaf`` flag of an option."""
    for entry in document()["argv"]:
        if entry["option"] == name and entry["flag"]:
            return str(entry["flag"])
    return str(document()["cli"][name])


def text(value: Any) -> str:
    """Command-line form of a value; the same text pkg/scoreopts.Text writes."""
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, float) and math.isfinite(value):
        if value == math.trunc(value) and abs(value) < 1e15:
            return str(int(value))
        return repr(value)
    return str(value)


def _number_reason(schema: dict[str, Any], value: Any) -> str | None:
    if isinstance(value, bool) or not isinstance(value, int | float):
        return "expected a number"
    if schema["type"] == "integer" and value != math.trunc(value):
        return "expected an integer"
    if "minimum" in schema and value < schema["minimum"]:
        return f"must be >= {text(schema['minimum'])}"
    if "maximum" in schema and value > schema["maximum"]:
        return f"must be <= {text(schema['maximum'])}"
    return None


def _scalar_reason(schema: dict[str, Any], value: Any) -> str | None:
    kind = schema["type"]
    if kind == "boolean":
        return None if isinstance(value, bool) else "expected a boolean"
    if kind == "string" and not isinstance(value, str):
        return "expected a string"
    if kind in ("integer", "number"):
        reason = _number_reason(schema, value)
        if reason:
            return reason
    if "enum" in schema and value not in schema["enum"]:
        return "must be one of " + "|".join(text(v) for v in schema["enum"])
    return None


def _list_reason(record: dict[str, Any], value: Any) -> str | None:
    if not isinstance(value, list | tuple) or not all(isinstance(v, str) for v in value):
        return "expected an array of strings"
    if len(value) > MAX_LIST_LEN:
        return f"at most {MAX_LIST_LEN} values"
    item = record["schema"].get("items", record["schema"])
    for entry in value:
        reason = "empty value" if not entry else _scalar_reason(item, entry)
        if reason:
            return reason
    return None


def check(record: dict[str, Any], value: Any, surface: str = "mcp") -> None:
    """Raise ValueError("invalid <name> <value>: <reason>") for a bad value."""
    listed = surface in record["repeat"] or record["type"] == "flags"
    reason = _list_reason(record, value) if listed else _scalar_reason(record["schema"], value)
    default = (record.get("surface_defaults") or {}).get(surface, record["default"])
    if reason is None and record["reserved"] and value != default:
        reason = f"{record['reserved']}; only {text(default)} is accepted"
    if reason:
        label = record["mcp"] if surface == "mcp" and record["mcp"] else record["name"]
        raise ValueError(f"invalid {label} {text(value)}: {reason}")


def from_mcp(arguments: dict[str, Any]) -> dict[str, Any]:
    """The scoring options of an MCP call, checked in definition order, by option name."""
    values: dict[str, Any] = {}
    for record in document()["options"]:
        name = record["mcp"]
        if not name or arguments.get(name) is None:
            continue
        value = arguments[name]
        check(record, value)
        values[record["name"]] = list(value) if isinstance(value, list | tuple) else value
    return values


def _entry_args(entry: dict[str, Any], value: Any) -> list[str]:
    form = entry["form"]
    if form == "switch":
        return [entry["flag"]] if value is True else []
    if form == "repeat":
        return [part for item in value for part in (entry["flag"], str(item))]
    if form == "choice":
        chosen = (entry["values"] or {}).get(text(value))
        return [chosen] if chosen else []
    if form == "value":
        return [entry["flag"], text(value)]
    return []


def extra_args(values: dict[str, Any]) -> list[str]:
    """The flags of every set option of argument-vector stage "extra", in definition order."""
    argv: list[str] = []
    for entry in document()["argv"]:
        if entry["stage"] == "extra" and values.get(entry["option"]) is not None:
            argv += _entry_args(entry, values[entry["option"]])
    return argv


def model_spec(model: str, values: dict[str, Any]) -> str:
    """``model`` (the library default when empty) with the suffixes of the set options."""
    if not model:
        model = "version=" + library_default("model")
    for entry in document()["argv"]:
        value = values.get(entry["option"])
        if entry["form"] != "suffix" or value is None or value is False:
            continue
        suffix = entry["suffix"] if value is True else entry["suffix"].replace("{}", text(value))
        if suffix not in model:
            model += suffix
    return model
