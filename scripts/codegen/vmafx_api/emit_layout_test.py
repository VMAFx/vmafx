# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""ABI layout test (core/test/test_vmafx_abi_layout.c).

Every struct size and alignment, every field offset, the size of every array
field (its element count), every enum value, flag bit and status value the
definition implies is a `_Static_assert` against the generated headers, so a
header that drifted from the definition, or a compiler that lays a struct out
differently, fails to compile. Nested structs are asserted on their own and
inside the struct that embeds them. Where the data models disagree, each gets
its own `_WIN64` branch. The Python binding checks the same numbers at import.
"""

from __future__ import annotations

from . import ctext
from .layout import DATA_MODELS, StructLayout, layouts, models_agree
from .model import Api
from .validate import constants


def _assert(condition: str, message: str) -> str:
    return ctext.packed("_Static_assert", [condition, f'"{message}"'], ";")


def _probe(name: str) -> str:
    return f"vmafx_layout_probe_{name}"


def _struct_asserts(api: Api, model: str) -> list[str]:
    out = []
    for name, lay in layouts(api, model).items():
        out.append(_assert(f"sizeof({name}) == {lay.size}", f"{name} size"))
        out.append(_assert(f"_Alignof({name}) == {lay.align}", f"{name} alignment"))
        out += _field_asserts(name, lay)
    return out


def _field_asserts(name: str, lay: StructLayout) -> list[str]:
    out = []
    for fld in lay.fields:
        condition = f"offsetof({name}, {fld.name}) == {fld.offset}"
        out.append(_assert(condition, f"{name}.{fld.name} offset"))
        if fld.count:
            condition = f"sizeof({_probe(name)}.{fld.name}) == {fld.size}"
            out.append(_assert(condition, f"{name}.{fld.name} holds {fld.count} elements"))
    return out


def _probes(api: Api) -> list[str]:
    """Declarations (never defined) whose members `sizeof` reads without evaluating them."""
    names = [s.name for s in api.structs if any(f.count for f in s.fields)]
    return [f"extern const {name} {_probe(name)};" for name in names]


def _constant_asserts(api: Api) -> list[str]:
    out = []
    for name, value in constants(api).items():
        if any(name == b.name for flags in api.flags for b in flags.bits):
            out.append(_assert(f"{name} == {hex(value)}u", name))
        else:
            out.append(_assert(f"{name} == {value}", name))
    return out


def _layout_section(api: Api) -> list[str]:
    if models_agree(api):
        return _struct_asserts(api, "LP64")
    out = ["#if defined(_WIN64) /* LLP64 */"]
    out += _struct_asserts(api, "LLP64")
    out.append("#else /* LP64 */")
    out += _struct_asserts(api, "LP64")
    return [*out, "#endif"]


def layout_test_source(api: Api) -> str:
    asserts = _layout_section(api) + _constant_asserts(api)
    n_fields = sum(len(item.fields) for item in api.structs)
    counts = [str(len(api.structs)), str(n_fields), str(len(constants(api)))]
    message = '"vmafx ABI layout: %d structs, %d fields, %d constants match the definition\\n"'
    report = ctext.packed("    (void)printf", [message, *counts], ";")
    probes = _probes(api)
    return (
        ctext.licence_block()
        + "\n"
        + ctext.banner_comment()
        + "\n"
        + "#include <stddef.h>\n#include <stdint.h>\n#include <stdio.h>\n\n"
        + f'#include "{api.umbrella.path}"\n\n'
        + '_Static_assert(sizeof(void *) == 8, "VMAFx builds 64-bit only (ADR-1258)");\n'
        + "".join(p + "\n" for p in probes)
        + "\n".join(asserts)
        + "\n\nint main(void)\n{\n"
        + report
        + "\n"
        + "    return 0;\n}\n"
    )


__all__ = ["DATA_MODELS", "layout_test_source"]
