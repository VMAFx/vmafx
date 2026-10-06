# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Data model of the VMAFx API definition (core/api/vmafx.toml, ADR-1852).

The definition is data only. `loader.py` builds these records from the TOML
document and `validate.py` checks them once, so an emitter never has to guess.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from typing import Any

# Scalar value types and their C spelling. `status` is VmafxStatus (int32_t).
SCALARS = {
    "u32": "uint32_t",
    "u64": "uint64_t",
    "i32": "int32_t",
    "i64": "int64_t",
    "f32": "float",
    "f64": "double",
    "uptr": "uintptr_t",
    "size": "size_t",
    "ptr": "void *",
    "status": "VmafxStatus",
    "cstr": "const char *",
}
VERSION_PARTS = 3  # MAJOR.MINOR.PATCH of [api] abi_version
SINCE_PARTS = 2  # MAJOR.MINOR of `since`, `removal`: one linker version node each
PASS_MODES = {"in", "in_const", "out", "out_handle", "error"}
HEADER_GROUPS = ("umbrella", "core", "optional")


class DefinitionError(ValueError):
    """The API definition is inconsistent; the message names the entry."""


@dataclass(frozen=True)
class Deprecation:
    since: tuple[int, int]
    replacement: str
    removal: tuple[int, int]


@dataclass(frozen=True)
class Header:
    path: str
    guard: str
    brief: str
    group: str
    includes: tuple[str, ...]

    @property
    def stem(self) -> str:
        """vmafx/context.h -> context."""
        return self.path.rsplit("/", 1)[-1].removesuffix(".h")


@dataclass(frozen=True)
class Status:
    name: str
    value: int
    errno: str
    reverse: bool
    doc: str
    since: tuple[int, int]
    deprecated: Deprecation | None


@dataclass(frozen=True)
class EnumValue:
    name: str
    value: int
    doc: str
    since: tuple[int, int]
    deprecated: Deprecation | None


@dataclass(frozen=True)
class Enum:
    name: str
    header: str
    doc: str
    since: tuple[int, int]
    deprecated: Deprecation | None
    values: tuple[EnumValue, ...]


@dataclass(frozen=True)
class FlagBit:
    name: str
    bit: int
    doc: str
    since: tuple[int, int]
    deprecated: Deprecation | None


@dataclass(frozen=True)
class Flags:
    """A `u32` / `u64` bitmask with named bits (no `bool` crosses the ABI)."""

    name: str
    header: str
    type: str
    doc: str
    since: tuple[int, int]
    deprecated: Deprecation | None
    bits: tuple[FlagBit, ...]


@dataclass(frozen=True)
class Handle:
    name: str
    header: str
    release: str
    doc: str
    since: tuple[int, int]
    deprecated: Deprecation | None


@dataclass(frozen=True)
class Field:
    name: str
    type: str
    doc: str
    since: tuple[int, int]
    deprecated: Deprecation | None
    enum: str = ""
    flags: str = ""
    count: int = 0  # 0: a single value; N > 0: a fixed array of N
    const: bool = False  # handle fields only: `const T *`

    def abi_key(self) -> tuple[object, ...]:
        """Everything about a field that the ABI depends on."""
        return (self.name, self.type, self.enum, self.flags, self.count, self.const)


@dataclass(frozen=True)
class Struct:
    name: str
    header: str
    sized: bool
    doc: str
    since: tuple[int, int]
    deprecated: Deprecation | None
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
class Callback:
    """A function-pointer type; its last parameter is `void *user`."""

    name: str
    header: str
    returns: str
    doc: str
    since: tuple[int, int]
    deprecated: Deprecation | None
    params: tuple[Param, ...]


@dataclass(frozen=True)
class Function:
    name: str
    header: str
    since: tuple[int, int]
    returns: str
    doc: str
    params: tuple[Param, ...]
    deprecated: Deprecation | None = None


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
class Option:
    """One option of an option group (design section 3.4); WP8 emits the surfaces."""

    name: str
    type: str
    default: object
    doc: str
    since: tuple[int, int]
    deprecated: Deprecation | None
    range: tuple[float, float] | None
    values: tuple[str, ...]
    spellings: dict[str, tuple[str, ...]]
    proto_field: int


@dataclass(frozen=True)
class OptionGroup:
    name: str
    doc: str
    since: tuple[int, int]
    surfaces: tuple[str, ...]
    options: tuple[Option, ...]


@dataclass(frozen=True)
class PixelFormat:
    """One row of the input format table (ADR-2145)."""

    enum: str
    name: str
    layout: str
    planar: str
    chroma: str
    siting: str
    planes: int
    bpc: tuple[int, int]
    shift: int
    msb: bool
    interleaved: bool
    packing: str
    elem: tuple[int, int, int]
    rgb_elems: int
    needs_statement: bool
    devices: tuple[str, ...]
    ffmpeg: dict[int, str]
    gstreamer: dict[int, str]


@dataclass(frozen=True)
class ColorMatrix:
    """One RGB to Y'CbCr matrix of the conversion table (ADR-2146)."""

    enum: str
    kr: str
    kb: str
    standard: str


@dataclass(frozen=True)
class Api:
    name: str
    abi_version: tuple[int, int, int]
    export_macro: str
    base_header: str
    version_header: str
    hide_unlisted: bool
    headers: tuple[Header, ...]
    statuses: tuple[Status, ...]
    enums: tuple[Enum, ...]
    flags: tuple[Flags, ...]
    handles: tuple[Handle, ...]
    callbacks: tuple[Callback, ...]
    structs: tuple[Struct, ...]
    functions: tuple[Function, ...]
    compats: tuple[Compat, ...]
    option_groups: tuple[OptionGroup, ...] = field(default=())
    pixel_formats: tuple[PixelFormat, ...] = field(default=())
    color_matrices: tuple[ColorMatrix, ...] = field(default=())

    @property
    def abi_minor_node(self) -> tuple[int, int]:
        return self.abi_version[0], self.abi_version[1]

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

    def header(self, path: str) -> Header:
        for item in self.headers:
            if item.path == path:
                return item
        raise DefinitionError(f"unknown header {path}")

    def is_handle(self, name: str) -> bool:
        return any(item.name == name for item in self.handles)

    def is_struct(self, name: str) -> bool:
        return any(item.name == name for item in self.structs)

    def is_enum(self, name: str) -> bool:
        return any(item.name == name for item in self.enums)

    def is_callback(self, name: str) -> bool:
        return any(item.name == name for item in self.callbacks)

    def flag_set(self, name: str) -> Flags | None:
        return next((item for item in self.flags if item.name == name), None)

    @property
    def umbrella(self) -> Header:
        return next(h for h in self.headers if h.group == "umbrella")


def upper_snake(name: str) -> str:
    """VmafxContextConfig -> VMAFX_CONTEXT_CONFIG."""
    return re.sub(r"(?<!^)(?=[A-Z])", "_", name).upper()


def version_text(version: tuple[int, ...]) -> str:
    return ".".join(str(part) for part in version)


def node_name(api: Api, since: tuple[int, int]) -> str:
    """Linker version node of an ABI minor: VMAFX_0.1."""
    return f"{api.name.upper()}_{since[0]}.{since[1]}"
