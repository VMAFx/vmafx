# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Build an Api from core/api/vmafx.toml (ADR-1852).

Every key an entry needs is read here; a missing one stops generation with a
message that names the entry. `validate.py` then checks the records as a
whole (types, headers, versions, uniqueness).
"""

from __future__ import annotations

from pathlib import Path

import tomllib

from .entries import Entry, deprecation, need, python_name, since_of, version, where_of
from .model import (
    VERSION_PARTS,
    Api,
    Callback,
    Compat,
    DefinitionError,
    Enum,
    EnumValue,
    Field,
    FlagBit,
    Flags,
    Function,
    Handle,
    Header,
    Param,
    ProtoRepeated,
    Status,
    Struct,
)
from .options import option_groups
from .validate import validate


def _headers(raw: list[Entry]) -> tuple[Header, ...]:
    out = []
    for entry in raw:
        where = where_of("headers", entry)
        out.append(
            Header(
                path=need(entry, "path", where),
                guard=need(entry, "guard", where),
                brief=need(entry, "brief", where),
                group=need(entry, "group", where),
                includes=tuple(entry.get("includes", [])),
            )
        )
    if not out:
        raise DefinitionError("at least one [[headers]] entry is required")
    return tuple(out)


def _statuses(raw: list[Entry]) -> tuple[Status, ...]:
    out = []
    for entry in raw:
        where = where_of("status", entry)
        out.append(
            Status(
                name=need(entry, "name", where),
                value=int(need(entry, "value", where)),
                errno=str(need(entry, "errno", where)),
                reverse=bool(entry.get("reverse", True)),
                doc=need(entry, "doc", where),
                since=since_of(entry, where),
                deprecated=deprecation(entry, where),
            )
        )
    if not out or out[0].name.split("_")[-1] != "OK" or out[0].value != 0:
        raise DefinitionError("the first status must be the OK code with value 0")
    return tuple(out)


def _enum_values(entry: Entry, where: str, since: tuple[int, int]) -> tuple[EnumValue, ...]:
    return tuple(
        EnumValue(
            name=need(v, "name", where),
            value=int(need(v, "value", where)),
            doc=v.get("doc", ""),
            since=since_of(v, f"{where}.{v.get('name')}", since),
            deprecated=deprecation(v, f"{where}.{v.get('name')}"),
        )
        for v in need(entry, "values", where)
    )


def _enums(raw: list[Entry]) -> tuple[Enum, ...]:
    out = []
    for entry in raw:
        where = where_of("enums", entry)
        since = since_of(entry, where)
        out.append(
            Enum(
                name=need(entry, "name", where),
                header=need(entry, "header", where),
                doc=entry.get("doc", ""),
                since=since,
                deprecated=deprecation(entry, where),
                values=_enum_values(entry, where, since),
            )
        )
    return tuple(out)


def _flag_bits(entry: Entry, where: str, since: tuple[int, int]) -> tuple[FlagBit, ...]:
    return tuple(
        FlagBit(
            name=need(b, "name", where),
            bit=int(need(b, "bit", where)),
            doc=need(b, "doc", f"{where}.{b.get('name')}"),
            since=since_of(b, f"{where}.{b.get('name')}", since),
            deprecated=deprecation(b, f"{where}.{b.get('name')}"),
        )
        for b in need(entry, "bits", where)
    )


def _flags(raw: list[Entry]) -> tuple[Flags, ...]:
    out = []
    for entry in raw:
        where = where_of("flags", entry)
        since = since_of(entry, where)
        out.append(
            Flags(
                name=need(entry, "name", where),
                header=need(entry, "header", where),
                type=need(entry, "type", where),
                doc=need(entry, "doc", where),
                since=since,
                deprecated=deprecation(entry, where),
                bits=_flag_bits(entry, where, since),
            )
        )
    return tuple(out)


def _handles(raw: list[Entry]) -> tuple[Handle, ...]:
    out = []
    for entry in raw:
        where = where_of("handles", entry)
        out.append(
            Handle(
                name=need(entry, "name", where),
                header=need(entry, "header", where),
                release=need(entry, "release", where),
                doc=entry.get("doc", ""),
                since=since_of(entry, where),
                deprecated=deprecation(entry, where),
            )
        )
    return tuple(out)


def _field(raw: Entry, where: str, since: tuple[int, int]) -> Field:
    where = f"{where}.{raw.get('name', '?')}"
    return Field(
        name=need(raw, "name", where),
        type=need(raw, "type", where),
        doc=raw.get("doc", ""),
        since=since_of(raw, where, since),
        deprecated=deprecation(raw, where),
        enum=raw.get("enum", ""),
        flags=raw.get("flags", ""),
        count=int(raw.get("count", 0)),
        const=bool(raw.get("const", False)),
    )


def _structs(raw: list[Entry]) -> tuple[Struct, ...]:
    out = []
    for entry in raw:
        where = where_of("structs", entry)
        since = since_of(entry, where)
        fields = [_field(f, where, since) for f in need(entry, "fields", where)]
        sized = bool(entry.get("sized", False))
        if any(f.name == "struct_size" for f in fields):
            raise DefinitionError(f"{where}: `struct_size` is implied by `sized = true`")
        if sized:
            size_doc = "Size of this struct as the caller compiled it; set by the _INIT macro."
            fields.insert(
                0, Field(name="struct_size", type="u32", doc=size_doc, since=since, deprecated=None)
            )
        out.append(
            Struct(
                name=need(entry, "name", where),
                header=need(entry, "header", where),
                sized=sized,
                doc=entry.get("doc", ""),
                since=since,
                deprecated=deprecation(entry, where),
                fields=tuple(fields),
                proto=str(entry.get("proto", "")),
                proto_repeated=_proto_repeated(entry, where),
            )
        )
    return tuple(out)


def _proto_repeated(entry: Entry, where: str) -> tuple[ProtoRepeated, ...]:
    return tuple(
        ProtoRepeated(
            name=need(raw, "name", where),
            struct=need(raw, "struct", where),
            number=int(need(raw, "number", where)),
            doc=str(raw.get("doc", "")),
        )
        for raw in entry.get("proto_repeated", [])
    )


def _params(entry: Entry, where: str) -> tuple[Param, ...]:
    return tuple(
        Param(
            name=need(p, "name", where),
            type=need(p, "type", where),
            mode=need(p, "pass", where),
            nullable=bool(p.get("nullable", False)),
        )
        for p in need(entry, "params", where)
    )


def _callbacks(raw: list[Entry]) -> tuple[Callback, ...]:
    out = []
    for entry in raw:
        where = where_of("callbacks", entry)
        out.append(
            Callback(
                name=need(entry, "name", where),
                header=need(entry, "header", where),
                returns=need(entry, "returns", where),
                doc=need(entry, "doc", where),
                since=since_of(entry, where),
                deprecated=deprecation(entry, where),
                params=_params(entry, where),
            )
        )
    return tuple(out)


def _functions(raw: list[Entry]) -> tuple[Function, ...]:
    out = []
    for entry in raw:
        where = where_of("functions", entry)
        out.append(
            Function(
                name=need(entry, "name", where),
                header=need(entry, "header", where),
                since=since_of(entry, where),
                returns=need(entry, "returns", where),
                doc=need(entry, "doc", where),
                params=_params(entry, where),
                deprecated=deprecation(entry, where),
                python=python_name(entry, where),
            )
        )
    return tuple(out)


def _compats(raw: list[Entry]) -> tuple[Compat, ...]:
    out = []
    for entry in raw:
        where = where_of("compat", entry)
        params = tuple(
            (need(p, "name", where), need(p, "c", where)) for p in need(entry, "params", where)
        )
        out.append(
            Compat(
                name=need(entry, "name", where),
                header=need(entry, "header", where),
                kind=need(entry, "kind", where),
                target=need(entry, "target", where),
                returns=need(entry, "returns", where),
                params=params,
                spec=dict(entry),
                engine_with=str(entry.get("engine_with", "")),
                until=str(entry.get("until", "")),
                when=str(entry.get("when", "")),
                file=str(entry.get("file", "")),
                calls=tuple(entry.get("calls", [])),
                note=str(entry.get("note", "")),
            )
        )
    return tuple(out)


def _abi_version(api: Entry) -> tuple[int, int, int]:
    major, minor, patch = version(need(api, "abi_version", "api"), "api.abi_version", VERSION_PARTS)
    return major, minor, patch


SCHEMA = 2  # [api] schema of this generator; 1 = the prototype definition (#2173)
MEMBER_TABLES = ("status", "enums", "handles", "structs", "functions")


def upgrade(document: Entry) -> Entry:
    """A schema-1 definition (the prototype) read as schema 2, for `--abi-check`.

    Schema 1 had no header groups, no per-entry headers and `since` on
    functions only. Its first header held every declaration, so it becomes the
    base, version and default header, a placeholder becomes the umbrella, and
    every entry without `since` gets the minor of the definition's ABI. Only
    the ABI-relevant content survives; header placement is not part of the ABI.
    """
    api = dict(need(document, "api", "top level"))
    if int(api.get("schema", 1)) == SCHEMA:
        return document
    upgraded = dict(document)
    headers = [dict(h, group="core") for h in document.get("headers", [])]
    first = headers[0]["path"]
    umbrella = {"path": f"{api['name']}/schema1-umbrella.h", "guard": "SCHEMA1", "brief": "-"}
    upgraded["headers"] = [*headers, dict(umbrella, group="umbrella")]
    major, minor, _ = str(api["abi_version"]).split(".")
    upgraded["api"] = dict(api, schema=SCHEMA, base_header=first, version_header=first)
    for table in MEMBER_TABLES:
        upgraded[table] = [
            {"since": f"{major}.{minor}", "header": first, **entry}
            for entry in document.get(table, [])
        ]
    return upgraded


def parse(document: Entry) -> Api:
    """Build and validate an Api from a parsed TOML document."""
    document = upgrade(document)
    api = need(document, "api", "top level")
    if int(api.get("schema", 0)) != SCHEMA:
        raise DefinitionError(f"api.schema must be {SCHEMA}")
    result = Api(
        name=need(api, "name", "api"),
        abi_version=_abi_version(api),
        export_macro=need(api, "export_macro", "api"),
        base_header=need(api, "base_header", "api"),
        version_header=need(api, "version_header", "api"),
        hide_unlisted=bool(api.get("hide_unlisted", False)),
        headers=_headers(document.get("headers", [])),
        statuses=_statuses(document.get("status", [])),
        enums=_enums(document.get("enums", [])),
        flags=_flags(document.get("flags", [])),
        handles=_handles(document.get("handles", [])),
        callbacks=_callbacks(document.get("callbacks", [])),
        structs=_structs(document.get("structs", [])),
        functions=_functions(document.get("functions", [])),
        compats=_compats(document.get("compat", [])),
        option_groups=option_groups(document.get("option_groups", [])),
    )
    validate(result)
    return result


def load(path: Path) -> Api:
    with path.open("rb") as handle:
        return parse(tomllib.load(handle))
