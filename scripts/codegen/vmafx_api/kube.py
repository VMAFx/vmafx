# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The Kubernetes part of the platform definition (ADR-2350 D13).

`[[groups]]` name an API group version and the Go package its types live in;
`[[resources]]` are its custom resources (kind, scope, short names, printer
columns, the status subresource, the spec and status messages). The spec and
status types are `[[messages]]` and `[[enums]]` entries that name a `group`
instead of a protobuf `file`: their fields carry the JSON name, an optional Go
name, and the OpenAPI validation controller-gen turns into the CRD schema
(`enum`, `minimum`, `min_length`, `max_items`, `pattern`, `format`, `default`,
`items_enum`, `items_min_length`). Within `v1` a resource only grows: the CRD
compatibility check of scripts/codegen/crd_generate.py refuses a removed
field, a new required field or narrower validation.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Any

from .entries import Entry, need, where_of
from .model import DefinitionError

SCALARS = {
    "string": "string",
    "bool": "bool",
    "int": "int",
    "int32": "int32",
    "int64": "int64",
    "float64": "float64",
    "Time": "*metav1.Time",
    "map<string, string>": "map[string]string",
}
VALIDATION_KEYS = (
    "enum",
    "items_enum",
    "minimum",
    "min_length",
    "items_min_length",
    "max_items",
    "pattern",
    "format",
)
COLUMN_TYPES = {"string", "number", "integer", "boolean", "date"}
DEFAULT_SCALARS = (str, bool, int, float)
JSON_NAME = re.compile(r"[a-z][A-Za-z0-9]*")
GO_NAME = re.compile(r"[A-Z][A-Za-z0-9]*")
ACRONYMS = {"id": "ID", "gpu": "GPU", "uri": "URI", "url": "URL", "oidc": "OIDC", "rbac": "RBAC"}


@dataclass(frozen=True)
class Group:
    name: str
    group: str
    version: str
    path: str
    go_package: str
    doc: str


@dataclass(frozen=True)
class KubeEnumValue:
    name: str
    doc: str


@dataclass(frozen=True)
class KubeEnum:
    name: str
    group: str
    doc: str
    values: tuple[KubeEnumValue, ...]


@dataclass(frozen=True)
class KubeField:
    name: str
    go: str
    type: str
    doc: str
    repeated: bool
    optional: bool
    pointer: bool
    validation: tuple[tuple[str, Any], ...]
    default: Any


@dataclass(frozen=True)
class KubeMessage:
    name: str
    group: str
    doc: str
    fields: tuple[KubeField, ...]


@dataclass(frozen=True)
class PrinterColumn:
    name: str
    type: str
    json_path: str


@dataclass(frozen=True)
class Resource:
    kind: str
    group: str
    doc: str
    short_names: tuple[str, ...]
    scope: str
    status_subresource: bool
    spec: str
    status: str
    spec_required: bool
    printer_columns: tuple[PrinterColumn, ...]

    @property
    def plural(self) -> str:
        return self.kind.lower() + "s"


@dataclass(frozen=True)
class Kube:
    groups: tuple[Group, ...]
    enums: tuple[KubeEnum, ...]
    messages: tuple[KubeMessage, ...]
    resources: tuple[Resource, ...]

    def group(self, name: str) -> Group:
        return next(g for g in self.groups if g.name == name)


def go_name(json_name: str) -> str:
    """The exported Go name of a JSON field name: tenantId -> TenantID."""
    parts = re.findall(r"[a-z0-9]+|[A-Z][a-z0-9]*", json_name)
    return "".join(ACRONYMS.get(p.lower(), p[:1].upper() + p[1:]) for p in parts)


def _text(entry: Entry, key: str, where: str) -> str:
    value = need(entry, key, where)
    if not isinstance(value, str) or not value.strip():
        raise DefinitionError(f"{where}: `{key}` is a non-empty string")
    return value.strip()


def _flag(entry: Entry, key: str, where: str, default: bool = False) -> bool:
    value = entry.get(key, default)
    if not isinstance(value, bool):
        raise DefinitionError(f"{where}: `{key}` is true or false")
    return value


def _groups(raw: list[Entry]) -> tuple[Group, ...]:
    out = []
    for entry in raw:
        where = where_of("groups", entry)
        out.append(
            Group(
                name=_text(entry, "name", where),
                group=_text(entry, "group", where),
                version=_text(entry, "version", where),
                path=_text(entry, "path", where),
                go_package=_text(entry, "go_package", where),
                doc=_text(entry, "doc", where),
            )
        )
    return tuple(out)


def _enum(entry: Entry) -> KubeEnum:
    where = where_of("enums", entry)
    values = tuple(
        KubeEnumValue(name=_text(v, "name", f"{where}.values"), doc=str(v.get("doc", "")).strip())
        for v in need(entry, "values", where)
    )
    return KubeEnum(
        name=_text(entry, "name", where),
        group=_text(entry, "group", where),
        doc=_text(entry, "doc", where),
        values=values,
    )


def _field(raw: Entry, where: str) -> KubeField:
    name = _text(raw, "name", where)
    return KubeField(
        name=name,
        go=str(raw.get("go", "")).strip() or go_name(name),
        type=_text(raw, "type", where),
        doc=_text(raw, "doc", where),
        repeated=_flag(raw, "repeated", where),
        optional=_flag(raw, "optional", where),
        pointer=_flag(raw, "pointer", where),
        validation=tuple((k, raw[k]) for k in VALIDATION_KEYS if k in raw),
        default=raw.get("default"),
    )


def _message(entry: Entry) -> KubeMessage:
    where = where_of("messages", entry)
    fields = tuple(
        _field(raw, f"{where}.fields[{raw.get('name', '?')}]")
        for raw in need(entry, "fields", where)
    )
    return KubeMessage(
        name=_text(entry, "name", where),
        group=_text(entry, "group", where),
        doc=_text(entry, "doc", where),
        fields=fields,
    )


def _columns(entry: Entry, where: str) -> tuple[PrinterColumn, ...]:
    out = []
    for raw in entry.get("printer_columns", []):
        at = f"{where}.printer_columns[{raw.get('name', '?')}]"
        column = PrinterColumn(
            name=_text(raw, "name", at),
            type=_text(raw, "type", at),
            json_path=_text(raw, "json_path", at),
        )
        if column.type not in COLUMN_TYPES:
            raise DefinitionError(f"{at}: type {column.type} is not one of {sorted(COLUMN_TYPES)}")
        out.append(column)
    return tuple(out)


def _resources(raw: list[Entry]) -> tuple[Resource, ...]:
    out = []
    for entry in raw:
        where = where_of("resources", {"name": entry.get("kind", "?")})
        out.append(
            Resource(
                kind=_text(entry, "kind", where),
                group=_text(entry, "group", where),
                doc=_text(entry, "doc", where),
                short_names=tuple(entry.get("short_names", [])),
                scope=str(entry.get("scope", "Namespaced")),
                status_subresource=_flag(entry, "status_subresource", where, True),
                spec=_text(entry, "spec", where),
                status=_text(entry, "status", where),
                spec_required=_flag(entry, "spec_required", where),
                printer_columns=_columns(entry, where),
            )
        )
    return tuple(out)


def parse_kube(data: dict[str, Any]) -> Kube:
    """The Kubernetes tables of the platform definition, validated."""
    kube = Kube(
        groups=_groups(data.get("groups", [])),
        enums=tuple(_enum(e) for e in data.get("enums", []) if "group" in e),
        messages=tuple(_message(m) for m in data.get("messages", []) if "group" in m),
        resources=_resources(data.get("resources", [])),
    )
    validate_kube(kube)
    return kube


# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------


def _duplicate(values: list[str]) -> str | None:
    seen: set[str] = set()
    for value in values:
        if value in seen:
            return value
        seen.add(value)
    return None


def _unique(label: str, values: list[str]) -> None:
    duplicate = _duplicate(values)
    if duplicate is not None:
        raise DefinitionError(f"{label}: {duplicate} appears twice")


def _known(kube: Kube, name: str) -> str:
    """'scalar', 'enum' or 'message' for a field type; refuses an unknown one."""
    if name in SCALARS:
        return "scalar"
    if any(e.name == name for e in kube.enums):
        return "enum"
    if any(m.name == name for m in kube.messages):
        return "message"
    raise DefinitionError(
        f"type {name} is neither a Kubernetes scalar nor a defined message or enum"
    )


def _check_field(kube: Kube, where: str, fld: KubeField) -> None:
    if not JSON_NAME.fullmatch(fld.name):
        raise DefinitionError(f"{where}: JSON names are lowerCamelCase")
    if not GO_NAME.fullmatch(fld.go):
        raise DefinitionError(f"{where}: Go name {fld.go} is not exported")
    kind = _known(kube, fld.type)
    if fld.pointer and (fld.repeated or fld.type.startswith("map<") or fld.type == "Time"):
        raise DefinitionError(f"{where}: `pointer` is for a single value")
    if fld.default is not None and not fld.optional:
        raise DefinitionError(f"{where}: a field with a default is optional")
    values = fld.default if isinstance(fld.default, list) else [fld.default]
    if fld.default is not None and not all(isinstance(v, DEFAULT_SCALARS) for v in values):
        raise DefinitionError(f"{where}: a default is a scalar or a list of scalars")
    if kind == "enum" and any(k == "enum" for k, _ in fld.validation):
        raise DefinitionError(f"{where}: an enum type carries its values; drop `enum`")


def _check_message(kube: Kube, message: KubeMessage) -> None:
    where = f"messages[{message.name}]"
    _unique(f"{where}: field name", [f.name for f in message.fields])
    _unique(f"{where}: Go name", [f.go for f in message.fields])
    for fld in message.fields:
        _check_field(kube, f"{where}.fields[{fld.name}]", fld)


def _check_resource(kube: Kube, resource: Resource) -> None:
    where = f"resources[{resource.kind}]"
    if not GO_NAME.fullmatch(resource.kind):
        raise DefinitionError(f"{where}: kind is an exported Go name")
    if resource.scope not in {"Namespaced", "Cluster"}:
        raise DefinitionError(f"{where}: scope is Namespaced or Cluster")
    for side in (resource.spec, resource.status):
        if _known(kube, side) != "message":
            raise DefinitionError(f"{where}: {side} is not a message")


def validate_kube(kube: Kube) -> None:
    groups = {g.name for g in kube.groups}
    _unique("groups: name", [g.name for g in kube.groups])
    names = [e.name for e in kube.enums] + [m.name for m in kube.messages]
    names += [r.kind for r in kube.resources] + [r.kind + "List" for r in kube.resources]
    _unique("Kubernetes types: name", names)
    owners = [(e.name, e.group) for e in kube.enums]
    owners += [(m.name, m.group) for m in kube.messages]
    owners += [(r.kind, r.group) for r in kube.resources]
    for name, group in owners:
        if group not in groups:
            raise DefinitionError(f"{name}: group {group} is not in [[groups]]")
    for enum in kube.enums:
        _unique(f"enums[{enum.name}]: value", [v.name for v in enum.values])
    for message in kube.messages:
        _check_message(kube, message)
    for resource in kube.resources:
        _check_resource(kube, resource)
