# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""OpenAPI components of the scoring API (design section 3.5, #2155).

The proto messages of `emit_proto` as OpenAPI 3.0 schemas. They are written
twice from one function: `api/openapi/components.gen.yaml` is the standalone
components document for integrators, and the same schemas are spliced into
`api/openapi/vmafx-server-v1.yaml` between generated markers, because the
server's code generator and its embedded `/openapi.json` need one
self-contained document. Property names are the option names, which are also
the proto field names.
"""

from __future__ import annotations

import json
from typing import Any

from .emit_mcp import value_schema
from .emit_proto import FIELD_TYPES, option_doc, repeated_message
from .model import Api, Field, Option, Struct, upper_snake
from .options import all_options

COMPONENTS_PATH = "api/openapi/components.gen.yaml"
SERVER_SPEC = "api/openapi/vmafx-server-v1.yaml"
BEGIN = "# BEGIN GENERATED: vmafx-api scoring schemas (scripts/codegen/vmafx-api.py)"
END = "# END GENERATED: vmafx-api scoring schemas"
SCHEMA_INDENT = 4  # schemas sit under `components:` / `schemas:`
MAX_YAML_NODES = 100_000  # bound of the iterative renderer (HISS-02)
INT32_MAX = 2_147_483_647
NUMBER_FORMATS = {"f32": "float", "f64": "double"}


def _scalar(value: object) -> str:
    """A YAML scalar; strings in JSON quoting, which YAML reads as is."""
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int | float):
        return repr(value)
    return json.dumps(str(value))


def _yaml(node: dict[str, Any], indent: int) -> list[str]:
    """Block YAML of nested dicts with scalar or flow-style list leaves.

    Iterative with an explicit stack (HISS-01: no recursion); a list leaf is
    written in flow style unless it holds scalars, which become `- item` lines.
    """
    out: list[str] = []
    stack: list[tuple[int, object]] = [(indent, node)]
    for _ in range(MAX_YAML_NODES):
        if not stack:
            return out
        depth, item = stack.pop()
        if isinstance(item, str):  # a rendered line queued in order
            out.append(item)
            continue
        assert isinstance(item, dict)
        stack.extend(reversed(_yaml_entries(item, depth)))
    raise ValueError("OpenAPI schema too large to render")


def _yaml_entries(node: dict[str, Any], depth: int) -> list[tuple[int, object]]:
    """The lines and nested mappings of one mapping, in order."""
    pad = " " * depth
    entries: list[tuple[int, object]] = []
    for key, value in node.items():
        if isinstance(value, dict) and value:
            entries += [(depth, f"{pad}{key}:"), (depth + 2, value)]
        elif (
            isinstance(value, list) and value and not any(isinstance(v, dict | list) for v in value)
        ):
            entries.append((depth, f"{pad}{key}:"))
            entries += [(depth, f"{pad}  - {_flow(v)}") for v in value]
        else:
            entries.append((depth, f"{pad}{key}: {_flow(value)}"))
    return entries


def _flow(value: object) -> str:
    if isinstance(value, dict | list):
        return json.dumps(value)
    return _scalar(value)


def _option_schema(option: Option) -> dict[str, Any]:
    schema = value_schema(option)
    if option.type in {"uint", "int"}:
        schema["format"] = "int64" if schema.get("maximum", 0) > INT32_MAX else "int32"
    if option.type == "float":
        schema["format"] = "double"
    if "proto" in option.repeat:
        schema = {"type": "array", "items": schema}
    schema["description"] = option_doc(option)
    return schema


def _field_schema(fld: Field) -> dict[str, Any]:
    if fld.enum:
        prefix = upper_snake(fld.enum) + "_"
        schema: dict[str, Any] = {"type": "string"}
        doc = f"{fld.doc} Value name of {fld.enum} without the {prefix} prefix, lower case."
    else:
        kind = FIELD_TYPES[fld.type]
        doc = fld.doc
        if kind == "string":
            schema = {"type": "string"}
        elif fld.type in NUMBER_FORMATS:
            schema = {"type": "number", "format": NUMBER_FORMATS[fld.type]}
        else:
            schema = {"type": "integer", "format": "int64" if "64" in kind else "int32"}
    if fld.count:
        schema = {"type": "array", "items": schema}
    schema["description"] = doc
    return schema


def schemas(api: Api) -> dict[str, dict[str, Any]]:
    """Component schemas: option messages, then struct messages."""
    out: dict[str, dict[str, Any]] = {}
    for group, option in all_options(api.option_groups):
        if not option.proto_field:
            continue
        message = out.setdefault(
            group.proto_message,
            {"type": "object", "description": "Scoring options.", "properties": {}},
        )
        message["properties"][option.name] = _option_schema(option)
    for struct in api.structs:
        if struct.proto:
            out[struct.proto] = _struct_schema(api, struct)
    return out


def _struct_schema(api: Api, struct: Struct) -> dict[str, Any]:
    fields = [f for f in struct.fields if f.name != "struct_size"]
    properties = {f.name: _field_schema(f) for f in fields}
    for member in struct.proto_repeated:
        message = repeated_message(api, struct, member)
        properties[member.name] = {
            "type": "array",
            "items": {"$ref": f"#/components/schemas/{message}"},
            "description": member.doc or f"{member.name} ({member.struct}).",
        }
    return {
        "type": "object",
        "description": f"{struct.doc} (from {struct.name}).",
        "properties": properties,
    }


def schema_lines(api: Api, indent: int = SCHEMA_INDENT) -> list[str]:
    return _yaml(schemas(api), indent)


def components_text(api: Api) -> str:
    """api/openapi/components.gen.yaml."""
    head = [
        "# GENERATED by scripts/codegen/vmafx-api.py from core/api/vmafx.toml (ADR-1852).",
        "# Do not edit: change the definition and run `python3 scripts/codegen/vmafx-api.py --write`.",
        "# " + "SPDX-" + "License-Identifier: EUPL-1.2",
        "#",
        "# OpenAPI 3.0 components of the versioned scoring API (#2155). The same",
        "# schemas are spliced into api/openapi/vmafx-server-v1.yaml.",
        "components:",
        "  schemas:",
    ]
    return "\n".join([*head, *schema_lines(api)]) + "\n"
