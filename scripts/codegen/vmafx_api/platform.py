# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The platform definition, api/vmafx-platform.toml (ADR-2350 D13).

Next to the core definition (core/api/vmafx.toml) the platform definition
describes the gRPC services and messages of the controller and the scoring
server: `[[files]]` (one protobuf file each), `[[enums]]`, `[[messages]]` and
`[[services]]`. A field names a protobuf scalar, `map<K, V>`, a message or enum
of this definition, or a message the core definition generates (an option
group's `proto_message`, a struct's `proto`). Field, value and RPC names and
numbers are the wire contract; they are written in the definition, never
derived, and only grow within a v1 package.
"""

from __future__ import annotations

import json
import re
from collections.abc import Hashable, Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import tomllib

from .chart import Chart, parse_chart
from .config import Config, parse_config
from .entries import Entry, need, where_of
from .kube import Kube, parse_kube
from .model import Api, DefinitionError

DEFINITION = Path("api/vmafx-platform.toml")
KUBERNETES_SUBSET = Path("api/kubernetes/openapi-subset.json")
PROTO_ROOT = "proto"
FORMAT_VERSION = 1
SCALARS = frozenset(
    {
        "double",
        "float",
        "int32",
        "int64",
        "uint32",
        "uint64",
        "sint32",
        "sint64",
        "fixed32",
        "fixed64",
        "sfixed32",
        "sfixed64",
        "bool",
        "string",
        "bytes",
    }
)
MAP_KEYS = SCALARS - {"double", "float", "bytes"}
MAP_TYPE = re.compile(r"map<\s*(\w+)\s*,\s*(\w+)\s*>")
IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
FIELD_MAX = 536_870_911
RESERVED_FIELDS = range(19_000, 20_000)


@dataclass(frozen=True)
class ProtoFile:
    name: str
    path: str
    package: str
    go_package: str
    doc: str

    @property
    def import_path(self) -> str:
        """The path other files import it by (relative to the buf module)."""
        return self.path.removeprefix(PROTO_ROOT + "/")


@dataclass(frozen=True)
class EnumValue:
    name: str
    number: int
    doc: str


@dataclass(frozen=True)
class PlatformEnum:
    name: str
    file: str
    doc: str
    values: tuple[EnumValue, ...]


@dataclass(frozen=True)
class MessageField:
    name: str
    type: str
    number: int
    doc: str
    repeated: bool
    oneof: str


@dataclass(frozen=True)
class Message:
    name: str
    file: str
    doc: str
    fields: tuple[MessageField, ...]


@dataclass(frozen=True)
class Rpc:
    name: str
    request: str
    response: str
    doc: str
    client_stream: bool
    server_stream: bool


@dataclass(frozen=True)
class Service:
    name: str
    file: str
    doc: str
    rpcs: tuple[Rpc, ...]


@dataclass(frozen=True)
class External:
    """A message another generator writes: its proto file and package."""

    name: str
    import_path: str
    package: str
    go_package: str


@dataclass(frozen=True)
class Platform:
    files: tuple[ProtoFile, ...]
    enums: tuple[PlatformEnum, ...]
    messages: tuple[Message, ...]
    services: tuple[Service, ...]
    external: tuple[External, ...]
    kube: Kube
    chart: Chart | None = None
    config: Config | None = None
    kubernetes: dict[str, Any] = field(default_factory=dict)

    def file(self, name: str) -> ProtoFile:
        return next(f for f in self.files if f.name == name)


def _text(entry: Entry, key: str, where: str) -> str:
    value = need(entry, key, where)
    if not isinstance(value, str) or not value.strip():
        raise DefinitionError(f"{where}: `{key}` is a non-empty string")
    return value.strip()


def _number(entry: Entry, where: str) -> int:
    value = need(entry, "number", where)
    if not isinstance(value, int) or isinstance(value, bool):
        raise DefinitionError(f"{where}: `number` is an integer")
    return value


def _flag(entry: Entry, key: str, where: str) -> bool:
    value = entry.get(key, False)
    if not isinstance(value, bool):
        raise DefinitionError(f"{where}: `{key}` is true or false")
    return value


def _files(raw: list[Entry]) -> tuple[ProtoFile, ...]:
    out = []
    for entry in raw:
        where = where_of("files", entry)
        out.append(
            ProtoFile(
                name=_text(entry, "name", where),
                path=_text(entry, "path", where),
                package=_text(entry, "package", where),
                go_package=_text(entry, "go_package", where),
                doc=_text(entry, "doc", where),
            )
        )
    return tuple(out)


def _enums(raw: list[Entry]) -> tuple[PlatformEnum, ...]:
    out = []
    for entry in raw:
        where = where_of("enums", entry)
        values = tuple(
            EnumValue(
                name=_text(v, "name", f"{where}.values"),
                number=_number(v, f"{where}.values[{v.get('name', '?')}]"),
                doc=str(v.get("doc", "")).strip(),
            )
            for v in need(entry, "values", where)
        )
        out.append(
            PlatformEnum(
                name=_text(entry, "name", where),
                file=_text(entry, "file", where),
                doc=_text(entry, "doc", where),
                values=values,
            )
        )
    return tuple(out)


def _fields(entry: Entry, where: str) -> tuple[MessageField, ...]:
    out = []
    for raw in need(entry, "fields", where):
        at = f"{where}.fields[{raw.get('name', '?')}]"
        out.append(
            MessageField(
                name=_text(raw, "name", at),
                type=_text(raw, "type", at),
                number=_number(raw, at),
                doc=str(raw.get("doc", "")).strip(),
                repeated=_flag(raw, "repeated", at),
                oneof=str(raw.get("oneof", "")).strip(),
            )
        )
    return tuple(out)


def _messages(raw: list[Entry]) -> tuple[Message, ...]:
    out = []
    for entry in raw:
        where = where_of("messages", entry)
        out.append(
            Message(
                name=_text(entry, "name", where),
                file=_text(entry, "file", where),
                doc=_text(entry, "doc", where),
                fields=_fields(entry, where),
            )
        )
    return tuple(out)


def _rpcs(entry: Entry, where: str) -> tuple[Rpc, ...]:
    out = []
    for raw in need(entry, "rpcs", where):
        at = f"{where}.rpcs[{raw.get('name', '?')}]"
        out.append(
            Rpc(
                name=_text(raw, "name", at),
                request=_text(raw, "request", at),
                response=_text(raw, "response", at),
                doc=_text(raw, "doc", at),
                client_stream=_flag(raw, "client_stream", at),
                server_stream=_flag(raw, "server_stream", at),
            )
        )
    return tuple(out)


def _services(raw: list[Entry]) -> tuple[Service, ...]:
    out = []
    for entry in raw:
        where = where_of("services", entry)
        out.append(
            Service(
                name=_text(entry, "name", where),
                file=_text(entry, "file", where),
                doc=_text(entry, "doc", where),
                rpcs=_rpcs(entry, where),
            )
        )
    return tuple(out)


def external_messages(
    api: Api, import_path: str, package: str, go_package: str
) -> tuple[External, ...]:
    """The messages the core definition generates into `import_path`."""
    names = {g.proto_message for g in api.option_groups}
    names |= {s.proto for s in api.structs if s.proto}
    return tuple(External(n, import_path, package, go_package) for n in sorted(names))


def parse(
    data: dict[str, Any],
    external: tuple[External, ...],
    kubernetes: dict[str, Any] | None = None,
) -> Platform:
    """A validated Platform from the definition's TOML tables; `kubernetes` is
    the `schemas` table of api/kubernetes/openapi-subset.json."""
    head = need(data, "platform", "platform definition")
    if need(head, "version", "[platform]") != FORMAT_VERSION:
        raise DefinitionError(f"[platform] version must be {FORMAT_VERSION}")
    platform = Platform(
        files=_files(data.get("files", [])),
        enums=_enums([e for e in data.get("enums", []) if "group" not in e]),
        messages=_messages([m for m in data.get("messages", []) if "group" not in m]),
        services=_services(data.get("services", [])),
        external=external,
        kube=parse_kube(data),
        chart=parse_chart(data),
        config=parse_config(data),
        kubernetes=dict(kubernetes or {}),
    )
    validate_platform(platform)
    kube_names = {e.name for e in platform.kube.enums} | {m.name for m in platform.kube.messages}
    proto_names = {e.name for e in platform.enums} | {m.name for m in platform.messages}
    clash = kube_names & proto_names
    if clash:
        raise DefinitionError(f"{sorted(clash)[0]} is both a protobuf and a Kubernetes type")
    return platform


def load_platform(path: Path, external: tuple[External, ...]) -> Platform:
    """The definition at `path`, with the Kubernetes subset beside it when present."""
    with path.open("rb") as fh:
        data = tomllib.load(fh)
    subset = path.parent / KUBERNETES_SUBSET.relative_to(DEFINITION.parent)
    kubernetes = None
    if subset.exists():
        kubernetes = json.loads(subset.read_text(encoding="utf-8"))["schemas"]
    return parse(data, external, kubernetes)


# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------


def _twice(values: Sequence[Hashable]) -> Hashable | None:
    seen: set[Hashable] = set()
    for value in values:
        if value in seen:
            return value
        seen.add(value)
    return None


def _unique(label: str, values: Sequence[Hashable]) -> None:
    duplicate = _twice(values)
    if duplicate is not None:
        raise DefinitionError(f"{label}: {duplicate} appears twice")


def _check_files(platform: Platform) -> None:
    _unique("files: name", [f.name for f in platform.files])
    _unique("files: path", [f.path for f in platform.files])
    for f in platform.files:
        want = f"{PROTO_ROOT}/{f.package.replace('.', '/')}/"
        if not (
            f.path.startswith(want) and f.path.endswith(".proto") and "/" not in f.path[len(want) :]
        ):
            raise DefinitionError(f"files[{f.name}]: path {f.path} must lie directly in {want}")
        if not re.fullmatch(r"[a-z][a-z0-9]*(\.[a-z][a-z0-9]*)*", f.package):
            raise DefinitionError(f"files[{f.name}]: package {f.package} is lower-case dotted")
        for ext in platform.external:
            if ext.package == f.package and ext.go_package != f.go_package:
                raise DefinitionError(
                    f"files[{f.name}]: package {f.package} is also generated into Go package "
                    f"{ext.go_package}; go_package must match"
                )


def _check_names(platform: Platform) -> None:
    files = {f.name for f in platform.files}
    named: list[PlatformEnum | Message | Service] = [
        *platform.enums,
        *platform.messages,
        *platform.services,
    ]
    _unique(
        "types and services: name", [n.name for n in named] + [e.name for e in platform.external]
    )
    for item in named:
        if not IDENTIFIER.fullmatch(item.name):
            raise DefinitionError(f"{item.name}: not an identifier")
        if item.file not in files:
            raise DefinitionError(f"{item.name}: file {item.file} is not in [[files]]")


def _check_enum(enum: PlatformEnum) -> None:
    where = f"enums[{enum.name}]"
    if not enum.values or enum.values[0].number != 0:
        raise DefinitionError(f"{where}: the first value has number 0")
    _unique(f"{where}: value name", [v.name for v in enum.values])
    _unique(f"{where}: value number", [v.number for v in enum.values])


def resolve(platform: Platform, name: str) -> tuple[str, ProtoFile | External | None]:
    """('scalar' | 'enum' | 'message', the file that defines it) of a type name."""
    if name in SCALARS:
        return "scalar", None
    for enum in platform.enums:
        if enum.name == name:
            return "enum", platform.file(enum.file)
    for message in platform.messages:
        if message.name == name:
            return "message", platform.file(message.file)
    for ext in platform.external:
        if ext.name == name:
            return "message", ext
    raise DefinitionError(f"type {name} is neither a scalar nor a defined message or enum")


def _check_field_type(platform: Platform, where: str, fld: MessageField) -> None:
    match = MAP_TYPE.fullmatch(fld.type)
    if match is None:
        resolve(platform, fld.type)
        return
    key, value = match.groups()
    if key not in MAP_KEYS:
        raise DefinitionError(f"{where}: map key {key} is not an integer, bool or string")
    if fld.repeated or fld.oneof:
        raise DefinitionError(f"{where}: a map is neither repeated nor in a oneof")
    resolve(platform, value)


def _check_field(platform: Platform, where: str, fld: MessageField) -> None:
    if not IDENTIFIER.fullmatch(fld.name) or fld.name.lower() != fld.name:
        raise DefinitionError(f"{where}: field names are lower_snake_case")
    if not 1 <= fld.number <= FIELD_MAX or fld.number in RESERVED_FIELDS:
        raise DefinitionError(f"{where}: number {fld.number} is not a valid field number")
    if fld.repeated and fld.oneof:
        raise DefinitionError(f"{where}: a oneof member is not repeated")
    _check_field_type(platform, where, fld)


def _check_message(platform: Platform, message: Message) -> None:
    where = f"messages[{message.name}]"
    _unique(f"{where}: field name", [f.name for f in message.fields])
    _unique(f"{where}: field number", [f.number for f in message.fields])
    for fld in message.fields:
        _check_field(platform, f"{where}.fields[{fld.name}]", fld)


def _check_service(platform: Platform, service: Service) -> None:
    where = f"services[{service.name}]"
    _unique(f"{where}: rpc name", [r.name for r in service.rpcs])
    for rpc in service.rpcs:
        for side in (rpc.request, rpc.response):
            kind, _ = resolve(platform, side)
            if kind != "message":
                raise DefinitionError(f"{where}.rpcs[{rpc.name}]: {side} is not a message")


def validate_platform(platform: Platform) -> None:
    _check_files(platform)
    _check_names(platform)
    for enum in platform.enums:
        _check_enum(enum)
    for message in platform.messages:
        _check_message(platform, message)
    for service in platform.services:
        _check_service(platform, service)
