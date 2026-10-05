# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""ABI layout test (core/test/test_vmafx_abi_layout.c).

Every struct size, field offset, enum value and status value the definition
implies is a `_Static_assert` against the generated header, so a header that
drifted from the definition, or a compiler that lays a struct out differently,
fails to compile. The Python binding checks the same numbers at import.
"""

from __future__ import annotations

from . import ctext
from .layout import struct_layout
from .model import Api


def _assert(condition: str, message: str) -> str:
    return ctext.packed("_Static_assert", [condition, f'"{message}"'], ";")


def _struct_asserts(api: Api) -> list[str]:
    out = []
    for item in api.structs:
        lay = struct_layout(item)
        out.append(_assert(f"sizeof({item.name}) == {lay.size}", f"{item.name} size"))
        out.extend(
            _assert(
                f"offsetof({item.name}, {f.name}) == {f.offset}", f"{item.name}.{f.name} offset"
            )
            for f in lay.fields
        )
    return out


def _constant_asserts(api: Api) -> list[str]:
    out = [_assert(f"{s.name} == {s.value}", s.name) for s in api.statuses]
    for enum in api.enums:
        out.extend(_assert(f"{v.name} == {v.value}", v.name) for v in enum.values)
    return out


def layout_test_source(api: Api) -> str:
    asserts = _struct_asserts(api) + _constant_asserts(api)
    n_fields = sum(len(item.fields) for item in api.structs)
    n_constants = len(api.statuses) + sum(len(e.values) for e in api.enums)
    counts = [str(len(api.structs)), str(n_fields), str(n_constants)]
    message = '"vmafx ABI layout: %d structs, %d fields, %d constants match the definition\\n"'
    report = ctext.packed("    (void)printf", [message, *counts], ";")
    return (
        ctext.licence_block()
        + "\n"
        + ctext.banner_comment()
        + "\n"
        + "#include <stddef.h>\n#include <stdint.h>\n#include <stdio.h>\n\n"
        + '#include "vmafx/vmafx.h"\n\n'
        + '_Static_assert(sizeof(void *) == 8, "VMAFx builds 64-bit only (ADR-1258)");\n'
        + "\n".join(asserts)
        + "\n\nint main(void)\n{\n"
        + report
        + "\n"
        + "    return 0;\n}\n"
    )
