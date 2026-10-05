# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Struct layouts the definition implies on the supported 64-bit targets.

VMAFx builds 64-bit only (ADR-1258). Struct fields are fixed-width scalars or
pointers, so LP64 (Linux, macOS) and LLP64 (Windows) agree. The C layout test
and every binding assert these numbers, so the definition, the compiler and the
binding must all agree before a build passes.
"""

from __future__ import annotations

from dataclasses import dataclass

from .model import Struct

SIZE_ALIGN = {"u32": (4, 4), "i32": (4, 4), "u64": (8, 8), "f64": (8, 8), "cstr": (8, 8)}


@dataclass(frozen=True)
class FieldLayout:
    name: str
    offset: int
    size: int


@dataclass(frozen=True)
class StructLayout:
    name: str
    size: int
    align: int
    fields: tuple[FieldLayout, ...]


def _round_up(value: int, align: int) -> int:
    return (value + align - 1) // align * align


def struct_layout(item: Struct) -> StructLayout:
    offset = 0
    align = 1
    fields = []
    for field in item.fields:
        size, field_align = SIZE_ALIGN[field.type]
        offset = _round_up(offset, field_align)
        fields.append(FieldLayout(name=field.name, offset=offset, size=size))
        offset += size
        align = max(align, field_align)
    return StructLayout(
        name=item.name, size=_round_up(offset, align), align=align, fields=tuple(fields)
    )
