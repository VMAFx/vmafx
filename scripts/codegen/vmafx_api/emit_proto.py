# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Protobuf messages of the scoring API (design section 3.5, #2155).

`proto/vmafx/v1/vmafx_api.proto` holds one message per `proto_message` of the
option groups (`ScoreOptions` by default) and one message per struct that names
`proto = "..."` (the provenance record). It sits next to the scoring service file
`proto/vmafx/v1/vmafx.proto` (generated from api/vmafx-platform.toml, ADR-2350),
in the same package and Go package, so the service imports it and `buf generate`
writes both into `gen/go`. Field numbers
come from the definition (`proto = { field = N }`) for options and from the
field order for structs, which only grow at the end; `buf breaking` and the
append-only checker refuse a renumbering. A struct's `proto_repeated` members
(RC4 WP5: the provenance record's models, features and annotations) are
repeated messages of other structs with explicit numbers from
PROTO_REPEATED_MIN, so they never meet the field-order numbers.
"""

from __future__ import annotations

import textwrap

from .model import (
    PROTO_REPEATED_MIN,
    Api,
    DefinitionError,
    Field,
    Option,
    ProtoRepeated,
    Struct,
    upper_snake,
)
from .options import PROTO_RESERVED, all_options

PATH = "proto/vmafx/v1/vmafx_api.proto"
IMPORT_PATH = "vmafx/v1/vmafx_api.proto"  # PATH relative to the buf module (proto/)
PACKAGE = "vmafx.v1"
# The Go package the bindings are in (gen/go, package vmafxv1).
GO_PACKAGE = "github.com/VMAFx/vmafx/gen/go;vmafxv1"
OPTION_TYPES = {
    "bool": "bool",
    "int": "int32",
    "uint": "uint32",
    "float": "double",
    "string": "string",
    "enum": "string",
}
FIELD_TYPES = {
    "u32": "uint32",
    "u64": "uint64",
    "i32": "int32",
    "i64": "int64",
    "f32": "float",
    "f64": "double",
    "cstr": "string",
    "size": "uint64",
}
UINT32_MAX = 4_294_967_295
INT32_MAX = 2_147_483_647


def _comment(text: str, indent: str = "") -> list[str]:
    return [f"{indent}// {line}" for line in textwrap.wrap(text, width=96 - len(indent))]


def _option_type(option: Option) -> str:
    """uint32 / int32 unless the option's maximum needs 64 bits."""
    kind = OPTION_TYPES.get(option.type, "string")
    high = option.range[1] if option.range else None
    if high is not None and option.type == "uint" and high > UINT32_MAX:
        return "uint64"
    if high is not None and option.type == "int" and high > INT32_MAX:
        return "int64"
    return kind


def option_doc(option: Option) -> str:
    text = option.doc
    if option.values:
        text += " One of: " + ", ".join(option.values) + "."
    if option.choices:
        text += " One of: " + ", ".join(str(c) for c in option.choices) + "."
    default = option.default_on("proto")
    if option.default_macro:
        text += f" Unset: the library default ({option.default_macro})."
    elif default is not None and default != []:
        text += f" Unset: {str(default).lower() if isinstance(default, bool) else default}."
    if option.reserved:
        text += f" Reserved: {option.reserved}; only the default is accepted."
    return text


def _option_field(option: Option) -> list[str]:
    listed = option.type == "flags" or "proto" in option.repeat
    label = "repeated " if listed else "optional "
    kind = "string" if option.type == "flags" else _option_type(option)
    return [
        *_comment(option_doc(option), "  "),
        f"  {label}{kind} {option.name} = {option.proto_field};",
    ]


def _messages(api: Api) -> dict[str, list[Option]]:
    out: dict[str, list[Option]] = {}
    for group, option in all_options(api.option_groups):
        if option.proto_field:
            out.setdefault(group.proto_message, []).append(option)
    return out


def _option_message(name: str, options: list[Option]) -> list[str]:
    lines = [
        *_comment(f"{name}: scoring options, generated from the option groups."),
        f"message {name} {{",
    ]
    for i, option in enumerate(sorted(options, key=lambda o: o.proto_field)):
        lines += ([""] if i else []) + _option_field(option)
    return [*lines, "}"]


def _struct_field(api: Api, struct: Struct, fld: Field, number: int) -> list[str]:
    if fld.enum:
        kind = "string"
        prefix = upper_snake(fld.enum) + "_"
        doc = f"{fld.doc} Value name of {fld.enum} without the {prefix} prefix, lower case."
    elif fld.type in FIELD_TYPES:
        kind, doc = FIELD_TYPES[fld.type], fld.doc
    else:
        raise DefinitionError(
            f"structs[{struct.name}].{fld.name}: a {fld.type} field has no proto form"
        )
    label = "repeated " if fld.count else ""
    return [*_comment(doc, "  "), f"  {label}{kind} {fld.name} = {number};"]


def repeated_message(api: Api, struct: Struct, member: ProtoRepeated) -> str:
    """Proto message of a struct's proto-only repeated member; refuses a bad member."""
    where = f"structs[{struct.name}].proto_repeated[{member.name}]"
    target = next((s for s in api.structs if s.name == member.struct), None)
    if target is None or not target.proto:
        raise DefinitionError(f"{where}: {member.struct} is not a struct with a proto message")
    if not PROTO_REPEATED_MIN <= member.number < PROTO_RESERVED[0]:
        raise DefinitionError(
            f"{where}: number {member.number} is outside {PROTO_REPEATED_MIN}.."
            f"{PROTO_RESERVED[0] - 1}"
        )
    return target.proto


def _twice(values: list[str]) -> str | None:
    return next((v for v in values if values.count(v) > 1), None)


def _check_repeated_names(struct: Struct) -> None:
    names = [f.name for f in struct.fields] + [m.name for m in struct.proto_repeated]
    numbers = [str(m.number) for m in struct.proto_repeated]
    for label, values in (("name", names), ("number", numbers)):
        duplicate = _twice(values)
        if duplicate is not None:
            raise DefinitionError(
                f"structs[{struct.name}].proto_repeated: {label} {duplicate} twice"
            )


def _repeated_fields(api: Api, struct: Struct) -> list[str]:
    _check_repeated_names(struct)
    lines: list[str] = []
    for member in struct.proto_repeated:
        message = repeated_message(api, struct, member)
        lines += [
            "",
            *_comment(member.doc or f"{member.name} ({member.struct}).", "  "),
            f"  repeated {message} {member.name} = {member.number};",
        ]
    return lines


def _struct_message(api: Api, struct: Struct) -> list[str]:
    fields = [f for f in struct.fields if f.name != "struct_size"]
    if len(fields) >= PROTO_REPEATED_MIN:
        raise DefinitionError(
            f"structs[{struct.name}]: {len(fields)} fields reach the proto_repeated numbers"
        )
    lines = [
        *_comment(f"{struct.proto}: {struct.doc} (from {struct.name})."),
        f"message {struct.proto} {{",
    ]
    for i, fld in enumerate(fields):
        lines += ([""] if i else []) + _struct_field(api, struct, fld, i + 1)
    return [*lines, *_repeated_fields(api, struct), "}"]


HEADER = """// GENERATED by scripts/codegen/vmafx-api.py from core/api/vmafx.toml (ADR-1852).
// Do not edit: change the definition and run `python3 scripts/codegen/vmafx-api.py --write`,
// then `python3 scripts/codegen/proto_generate.py --write` (gen/go/AGENTS.md).
//
// {spdx}
// Copyright 2026 Lusoris
//
// Messages of the versioned scoring API (#2155): the options every request may
// carry and the provenance record every response carries. The services that use
// them are generated from api/vmafx-platform.toml into vmafx/v1/vmafx.proto.

syntax = "proto3";

package {package};

option go_package = "{go_package}";
"""


def proto_text(api: Api) -> str:
    header = HEADER.format(
        spdx="SPDX-" + "License-Identifier: EUPL-1.2", package=PACKAGE, go_package=GO_PACKAGE
    )
    orphan = next((s for s in api.structs if s.proto_repeated and not s.proto), None)
    if orphan is not None:
        raise DefinitionError(f"structs[{orphan.name}]: proto_repeated without a proto message")
    blocks = [_option_message(n, o) for n, o in _messages(api).items()]
    blocks += [_struct_message(api, s) for s in api.structs if s.proto]
    body = "\n\n".join("\n".join(block) for block in blocks)
    return header + ("\n" + body + "\n" if body else "")
