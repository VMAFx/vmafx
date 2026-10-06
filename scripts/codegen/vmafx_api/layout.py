# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Struct layouts the definition implies, per data model.

VMAFx builds 64-bit only (ADR-1258): LP64 (Linux, macOS) and LLP64
(Windows). Fields are fixed-width scalars, pointers (`ptr`, `cstr`, handles,
callbacks), `size_t`, `uintptr_t`, nested structs and fixed arrays of any of
these, so both models give the same numbers; the computer still takes the
model as data, so a type whose size differs between them would show up as two
layouts and the C test would assert each under its own `_WIN64` branch. The C
layout test and every binding assert these numbers, so the definition, the
compiler and the binding must agree before a build passes.
"""

from __future__ import annotations

from dataclasses import dataclass

from . import graph, typesys
from .model import Api, Field, Struct

POINTER = (8, 8)
# (size, alignment) per scalar; pointers, handles and callbacks use POINTER.
DATA_MODELS: dict[str, dict[str, tuple[int, int]]] = {
    "LP64": {
        "u32": (4, 4),
        "i32": (4, 4),
        "status": (4, 4),
        "f32": (4, 4),
        "u64": (8, 8),
        "i64": (8, 8),
        "f64": (8, 8),
        "uptr": (8, 8),
        "size": (8, 8),
        "ptr": POINTER,
        "cptr": POINTER,
        "cstr": POINTER,
    },
}
DATA_MODELS["LLP64"] = dict(DATA_MODELS["LP64"])


@dataclass(frozen=True)
class FieldLayout:
    name: str
    offset: int
    size: int  # of the whole field (all elements of an array)
    count: int


@dataclass(frozen=True)
class StructLayout:
    name: str
    size: int
    align: int
    fields: tuple[FieldLayout, ...]


def _round_up(value: int, align: int) -> int:
    return (value + align - 1) // align * align


def _element(
    api: Api, fld: Field, scalars: dict[str, tuple[int, int]], done: dict[str, StructLayout]
) -> tuple[int, int]:
    kind = typesys.kind(api, fld.type)
    if kind == "struct":
        nested = done[fld.type]
        return nested.size, nested.align
    if kind in ("handle", "callback"):
        return scalars["ptr"]
    return scalars[fld.type]


def _one(
    api: Api, item: Struct, scalars: dict[str, tuple[int, int]], done: dict[str, StructLayout]
) -> StructLayout:
    offset, align, fields = 0, 1, []
    for fld in item.fields:
        size, field_align = _element(api, fld, scalars, done)
        size *= max(fld.count, 1)
        offset = _round_up(offset, field_align)
        fields.append(FieldLayout(name=fld.name, offset=offset, size=size, count=fld.count))
        offset += size
        align = max(align, field_align)
    return StructLayout(
        name=item.name, size=_round_up(offset, align), align=align, fields=tuple(fields)
    )


def by_value_order(api: Api) -> list[Struct]:
    """Every struct after the structs it embeds by value (definition order otherwise)."""
    by_name = {s.name: s for s in api.structs}
    needs = {s.name: {f.type for f in s.fields if f.type in by_name} for s in api.structs}
    return [by_name[name] for name in graph.order(list(by_name), needs, "by-value struct")]


def layouts(api: Api, model: str = "LP64") -> dict[str, StructLayout]:
    """Layout of every struct, nested structs first (no recursion)."""
    scalars = DATA_MODELS[model]
    done: dict[str, StructLayout] = {}
    for item in by_value_order(api):
        done[item.name] = _one(api, item, scalars, done)
    return {s.name: done[s.name] for s in api.structs}


def struct_layout(api: Api, item: Struct, model: str = "LP64") -> StructLayout:
    return layouts(api, model)[item.name]


def models_agree(api: Api) -> bool:
    """True when every data model gives every struct the same layout."""
    first, *rest = (layouts(api, model) for model in DATA_MODELS)
    return all(other == first for other in rest)
