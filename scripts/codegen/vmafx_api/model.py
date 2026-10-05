# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Load and validate the VMAFx API definition (core/api/vmafx.toml, ADR-1852).

The definition is data only. Everything an emitter needs is checked here once,
so an emitter never has to guess: unknown types, a struct field before
`struct_size`, a compat field map that misses a field of the struct it fills,
a duplicate symbol or constant value all stop generation with a message that
names the entry.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import tomllib

SCALARS = {
    "u32": "uint32_t",
    "u64": "uint64_t",
    "i32": "int32_t",
    "f64": "double",
    "status": "VmafxStatus",
    "cstr": "const char *",
}
VERSION_PARTS = 3  # MAJOR.MINOR.PATCH
PASS_MODES = {"in", "in_const", "out", "out_handle", "error"}


class DefinitionError(ValueError):
    """The API definition is inconsistent; the message names the entry."""


@dataclass(frozen=True)
class Header:
    path: str
    guard: str
    brief: str
    includes: tuple[str, ...]


@dataclass(frozen=True)
class Status:
    name: str
    value: int
    errno: str
    reverse: bool
    doc: str


@dataclass(frozen=True)
class EnumValue:
    name: str
    value: int


@dataclass(frozen=True)
class Enum:
    name: str
    doc: str
    values: tuple[EnumValue, ...]


@dataclass(frozen=True)
class Handle:
    name: str
    release: str
    doc: str


@dataclass(frozen=True)
class Field:
    name: str
    type: str
    enum: str
    doc: str


@dataclass(frozen=True)
class Struct:
    name: str
    sized: bool
    doc: str
    fields: tuple[Field, ...]

    @property
    def init_macro(self) -> str:
        return upper_snake(self.name) + "_INIT"


@dataclass(frozen=True)
class Param:
    name: str
    type: str
    mode: str
    nullable: bool


@dataclass(frozen=True)
class Function:
    name: str
    header: str
    since: str
    returns: str
    doc: str
    params: tuple[Param, ...]


@dataclass(frozen=True)
class Compat:
    name: str
    header: str
    kind: str
    target: str
    returns: str
    params: tuple[tuple[str, str], ...]
    spec: dict[str, Any]


@dataclass(frozen=True)
class Api:
    name: str
    abi_version: tuple[int, int, int]
    export_macro: str
    headers: tuple[Header, ...]
    statuses: tuple[Status, ...]
    enums: tuple[Enum, ...]
    handles: tuple[Handle, ...]
    structs: tuple[Struct, ...]
    functions: tuple[Function, ...]
    compats: tuple[Compat, ...]

    def struct(self, name: str) -> Struct:
        for item in self.structs:
            if item.name == name:
                return item
        raise DefinitionError(f"unknown struct {name}")

    def function(self, name: str) -> Function:
        for item in self.functions:
            if item.name == name:
                return item
        raise DefinitionError(f"unknown function {name}")

    def is_handle(self, name: str) -> bool:
        return any(item.name == name for item in self.handles)

    def is_struct(self, name: str) -> bool:
        return any(item.name == name for item in self.structs)

    def is_enum(self, name: str) -> bool:
        return any(item.name == name for item in self.enums)


def upper_snake(name: str) -> str:
    """VmafxContextConfig -> VMAFX_CONTEXT_CONFIG."""
    return re.sub(r"(?<!^)(?=[A-Z])", "_", name).upper()


def _need(entry: dict[str, Any], key: str, where: str) -> Any:
    if key not in entry:
        raise DefinitionError(f"{where}: missing `{key}`")
    return entry[key]


def _version(text: str) -> tuple[int, int, int]:
    parts = text.split(".")
    if len(parts) != VERSION_PARTS or not all(p.isdigit() for p in parts):
        raise DefinitionError(f"api.abi_version must be MAJOR.MINOR.PATCH, got {text!r}")
    return int(parts[0]), int(parts[1]), int(parts[2])


def _headers(raw: list[dict[str, Any]]) -> tuple[Header, ...]:
    out = []
    for entry in raw:
        where = f"headers[{entry.get('path', '?')}]"
        out.append(
            Header(
                path=_need(entry, "path", where),
                guard=_need(entry, "guard", where),
                brief=_need(entry, "brief", where),
                includes=tuple(entry.get("includes", [])),
            )
        )
    if not out:
        raise DefinitionError("at least one [[headers]] entry is required")
    return tuple(out)


def _statuses(raw: list[dict[str, Any]]) -> tuple[Status, ...]:
    out = []
    for entry in raw:
        where = f"status[{entry.get('name', '?')}]"
        out.append(
            Status(
                name=_need(entry, "name", where),
                value=int(_need(entry, "value", where)),
                errno=str(_need(entry, "errno", where)),
                reverse=bool(entry.get("reverse", True)),
                doc=_need(entry, "doc", where),
            )
        )
    if not out or out[0].name.split("_")[-1] != "OK" or out[0].value != 0:
        raise DefinitionError("the first status must be the OK code with value 0")
    return tuple(out)


def _enums(raw: list[dict[str, Any]]) -> tuple[Enum, ...]:
    out = []
    for entry in raw:
        where = f"enums[{entry.get('name', '?')}]"
        values = tuple(
            EnumValue(name=_need(v, "name", where), value=int(_need(v, "value", where)))
            for v in _need(entry, "values", where)
        )
        out.append(Enum(name=_need(entry, "name", where), doc=entry.get("doc", ""), values=values))
    return tuple(out)


def _handles(raw: list[dict[str, Any]]) -> tuple[Handle, ...]:
    return tuple(
        Handle(
            name=_need(e, "name", "handles"),
            release=_need(e, "release", f"handles[{e.get('name')}]"),
            doc=e.get("doc", ""),
        )
        for e in raw
    )


def _structs(raw: list[dict[str, Any]]) -> tuple[Struct, ...]:
    out = []
    for entry in raw:
        where = f"structs[{entry.get('name', '?')}]"
        fields = [
            Field(
                name=_need(f, "name", where),
                type=_need(f, "type", where),
                enum=f.get("enum", ""),
                doc=f.get("doc", ""),
            )
            for f in _need(entry, "fields", where)
        ]
        sized = bool(entry.get("sized", False))
        if any(f.name == "struct_size" for f in fields):
            raise DefinitionError(f"{where}: `struct_size` is implied by `sized = true`")
        if sized:
            size_doc = "Size of this struct as the caller compiled it; set by the _INIT macro."
            fields.insert(0, Field(name="struct_size", type="u32", enum="", doc=size_doc))
        out.append(
            Struct(name=entry["name"], sized=sized, doc=entry.get("doc", ""), fields=tuple(fields))
        )
    return tuple(out)


def _functions(raw: list[dict[str, Any]], default_header: str) -> tuple[Function, ...]:
    out = []
    for entry in raw:
        where = f"functions[{entry.get('name', '?')}]"
        params = tuple(
            Param(
                name=_need(p, "name", where),
                type=_need(p, "type", where),
                mode=_need(p, "pass", where),
                nullable=bool(p.get("nullable", False)),
            )
            for p in _need(entry, "params", where)
        )
        out.append(
            Function(
                name=_need(entry, "name", where),
                header=entry.get("header", default_header),
                since=_need(entry, "since", where),
                returns=_need(entry, "returns", where),
                doc=_need(entry, "doc", where),
                params=params,
            )
        )
    return tuple(out)


def _compats(raw: list[dict[str, Any]]) -> tuple[Compat, ...]:
    out = []
    for entry in raw:
        where = f"compat[{entry.get('name', '?')}]"
        params = tuple(
            (_need(p, "name", where), _need(p, "c", where)) for p in _need(entry, "params", where)
        )
        out.append(
            Compat(
                name=_need(entry, "name", where),
                header=_need(entry, "header", where),
                kind=_need(entry, "kind", where),
                target=_need(entry, "target", where),
                returns=_need(entry, "returns", where),
                params=params,
                spec=dict(entry),
            )
        )
    return tuple(out)


def parse(document: dict[str, Any]) -> Api:
    """Build and validate an Api from a parsed TOML document."""
    api = _need(document, "api", "top level")
    headers = _headers(document.get("headers", []))
    result = Api(
        name=_need(api, "name", "api"),
        abi_version=_version(_need(api, "abi_version", "api")),
        export_macro=_need(api, "export_macro", "api"),
        headers=headers,
        statuses=_statuses(document.get("status", [])),
        enums=_enums(document.get("enums", [])),
        handles=_handles(document.get("handles", [])),
        structs=_structs(document.get("structs", [])),
        functions=_functions(document.get("functions", []), headers[0].path),
        compats=_compats(document.get("compat", [])),
    )
    validate(result)
    return result


def load(path: Path) -> Api:
    with path.open("rb") as handle:
        return parse(tomllib.load(handle))


def _unique(names: list[str], what: str) -> None:
    seen: set[str] = set()
    for name in names:
        if name in seen:
            raise DefinitionError(f"duplicate {what}: {name}")
        seen.add(name)


def _check_type(api: Api, type_name: str, where: str) -> None:
    if type_name in SCALARS:
        return
    if type_name.startswith(("foreign:", "handle:")):
        return
    if api.is_handle(type_name) or api.is_struct(type_name) or api.is_enum(type_name):
        return
    raise DefinitionError(f"{where}: unknown type {type_name!r}")


def _check_fields(api: Api, item: Struct) -> None:
    for field in item.fields:
        where = f"structs[{item.name}].{field.name}"
        if field.type not in ("u32", "u64", "i32", "f64", "cstr"):
            raise DefinitionError(f"{where}: struct fields are fixed-width scalars or cstr")
        if field.enum and (field.type != "u32" or not api.is_enum(field.enum)):
            raise DefinitionError(f"{where}: `enum` needs type u32 and a declared enum")


def _check_function(api: Api, fn: Function, header_paths: set[str]) -> None:
    where = f"functions[{fn.name}]"
    if fn.header not in header_paths:
        raise DefinitionError(f"{where}: header {fn.header} is not declared")
    if fn.returns != "void":
        _check_type(api, fn.returns, f"{where}.returns")
    for param in fn.params:
        if param.mode not in PASS_MODES:
            raise DefinitionError(f"{where}.{param.name}: unknown pass mode {param.mode!r}")
        _check_type(api, param.type, f"{where}.{param.name}")
    has_error = any(p.mode == "error" for p in fn.params)
    if has_error and (fn.returns != "status" or fn.params[-1].mode != "error"):
        raise DefinitionError(f"{where}: the error out-parameter is last and the result a status")


def _check_compat(api: Api, item: Compat) -> None:
    where = f"compat[{item.name}]"
    if item.kind not in ("shim", "glue"):
        raise DefinitionError(f"{where}: generated kinds are shim and glue")
    api.function(item.target)
    build = item.spec.get("build")
    if build is None:
        return
    target = api.struct(build["type"])
    wanted = {f.name for f in target.fields if f.name != "struct_size"}
    given = set(build.get("from", {}))
    if wanted != given:
        missing = sorted(wanted - given)
        extra = sorted(given - wanted)
        raise DefinitionError(
            f"{where}: field map of {target.name} missing {missing} extra {extra}"
        )


def validate(api: Api) -> None:
    _unique([s.name for s in api.statuses], "status")
    _unique([str(s.value) for s in api.statuses], "status value")
    _unique([f.name for f in api.functions] + [c.name for c in api.compats], "function")
    _unique([h.name for h in api.handles] + [s.name for s in api.structs], "type")
    for enum in api.enums:
        _unique([v.name for v in enum.values], f"constant of {enum.name}")
        _unique([str(v.value) for v in enum.values], f"value of {enum.name}")
    for struct in api.structs:
        _check_fields(api, struct)
    header_paths = {h.path for h in api.headers}
    for fn in api.functions:
        _check_function(api, fn, header_paths)
    for compat in api.compats:
        _check_compat(api, compat)
