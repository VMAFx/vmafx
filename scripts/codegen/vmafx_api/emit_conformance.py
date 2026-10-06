# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Conformance tables of the libvmaf compat layer (ADR-1852 design section 2.11).

The conformance test (core/test/test_compat_conformance.c) runs every scenario
twice: once through the libvmaf functions as the engine defines them (the old
libvmaf, `vmaf_engine_<stem>`), once through the compat library on the VMAFx
API, and requires the two traces to be equal. These files give it the two
function tables and the coverage rule:

- `compat_conformance_gen.h`: `VmafCompatApi`, one pointer per compat function
  this build has, with its index and name.
- `compat_conformance_table_gen.c`: a counting thunk per function and the
  table of them. Meson compiles it twice: with the engine names
  (`VMAF_COMPAT_SIDE=old`) and with the libvmaf names (`VMAF_PUBLIC_NAMES`,
  `VMAF_COMPAT_SIDE=new`).

A function a scenario never called through both tables fails the test, so a
new compat function without a conformance case cannot pass. An `engine`
exception has no compat definition (the engine's function is the only one) and
is reported as such.
"""

from __future__ import annotations

from . import ctext
from .emit_compat import condition
from .model import Api, Compat

HEADER_PATH = "core/test/compat_conformance_gen.h"
TABLE_PATH = "core/test/compat_conformance_table_gen.c"
LIBVMAF_HEADERS = (
    "libvmaf/dnn.h",
    "libvmaf/feature.h",
    "libvmaf/libvmaf.h",
    "libvmaf/libvmaf_hip.h",
    "libvmaf/libvmaf_mcp.h",
    "libvmaf/libvmaf_metal.h",
    "libvmaf/model.h",
    "libvmaf/perceptual_weight.h",
    "libvmaf/picture.h",
    "libvmaf/picture_v2.h",
)


def _wrapped(text: str, cond: str) -> str:
    return f"#if {cond}\n{text}#endif\n" if cond else text


def tabled(api: Api) -> list[Compat]:
    """Compat functions with a compat definition (every kind but `engine`)."""
    return [item for item in api.compats if item.kind != "engine"]


def _index_name(item: Compat) -> str:
    return f"VMAF_COMPAT_{item.stem.upper()}"


def _pointer_field(item: Compat) -> str:
    params = [ctext.pointer_join(c, n) for n, c in item.params] or ["void"]
    head = ctext.pointer_join(item.returns, f"(*{item.stem})")
    return ctext.packed(f"    {head}", params, ";")


def header_text(api: Api) -> str:
    items = tabled(api)
    index = "".join(f"    {_index_name(i)},\n" for i in items)
    fields = "".join(_wrapped(_pointer_field(i) + "\n", condition(i)) for i in items)
    engine = [c for c in api.compats if c.kind == "engine"]
    engine_rows = "".join(f'    {{"{c.name}", "{c.engine_with}"}},\n' for c in engine)
    includes = "".join(f'#include "{h}"\n' for h in LIBVMAF_HEADERS)
    return (
        ctext.licence_block()
        + "\n"
        + ctext.banner_comment()
        + "\n#ifndef VMAF_COMPAT_CONFORMANCE_GEN_H\n#define VMAF_COMPAT_CONFORMANCE_GEN_H\n\n"
        + "#include <stddef.h>\n#include <stdint.h>\n\n"
        + includes
        + "\n/* Index of every compat function with a compat definition. */\n"
        + "enum VmafCompatIndex {\n"
        + index
        + "    VMAF_COMPAT_COUNT,\n};\n\n"
        + "/* The functions of one side; a member this build lacks is absent. */\n"
        + "typedef struct VmafCompatApi {\n"
        + "    unsigned count; /* VMAF_COMPAT_COUNT */\n"
        + fields
        + "} VmafCompatApi;\n\n"
        + '/* Name and build condition ("" = every build) of each index. */\n'
        + "typedef struct VmafCompatEntry {\n    const char *name;\n    const char *condition;\n"
        + "    int built;\n} VmafCompatEntry;\n\n"
        + "/* An `engine` exception: no compat definition while its backend is built\n"
        + " * (core/api/vmafx.toml names what ends it). */\n"
        + "typedef struct VmafCompatEngineOnly {\n    const char *name;\n    const char *backend;\n"
        + "} VmafCompatEngineOnly;\n\n"
        + f"#define VMAF_COMPAT_ENGINE_ONLY_COUNT {len(engine)}u\n\n"
        + "/* Each table ends with a NULL-named sentinel row. */\n"
        + "extern const VmafCompatEntry vmaf_compat_entries[VMAF_COMPAT_COUNT + 1];\n"
        + "extern const VmafCompatEngineOnly vmaf_compat_engine_only[VMAF_COMPAT_ENGINE_ONLY_COUNT + 1];\n"
        + "#define VMAF_COMPAT_ENTRY(name, cond) {name, #cond, cond}\n"
        + "extern const VmafCompatApi vmaf_compat_old;\n"
        + "extern const VmafCompatApi vmaf_compat_new;\n"
        + "extern unsigned vmaf_compat_calls_old[VMAF_COMPAT_COUNT];\n"
        + "extern unsigned vmaf_compat_calls_new[VMAF_COMPAT_COUNT];\n\n"
        + "#ifdef VMAF_COMPAT_ENTRIES_DEFINE\n"
        + "const VmafCompatEntry vmaf_compat_entries[VMAF_COMPAT_COUNT + 1] = {\n"
        + "".join(_entry_row(i) for i in items)
        + "    {NULL, NULL, 0},\n};\n"
        + "const VmafCompatEngineOnly vmaf_compat_engine_only[VMAF_COMPAT_ENGINE_ONLY_COUNT + 1] = {\n"
        + engine_rows
        + "    {NULL, NULL},\n};\n#endif\n\n"
        + "#endif /* VMAF_COMPAT_CONFORMANCE_GEN_H */\n"
    )


def _entry_row(item: Compat) -> str:
    """A row of the entries table; the condition is kept as text and as a value."""
    return f'    VMAF_COMPAT_ENTRY("{item.name}", {condition(item) or "1"}),\n'


def _thunk(number: int, item: Compat) -> str:
    """A counting forwarder named by its index, so every signature stays short."""
    params = [ctext.pointer_join(c, n) for n, c in item.params] or ["void"]
    signature = ctext.packed(
        ctext.pointer_join("static " + item.returns, f"VMAF_COMPAT_THUNK({number})"), params, ""
    )
    args = [n for n, _ in item.params]
    body = f"    VMAF_COMPAT_CALLS[{_index_name(item)}]++;\n"
    if item.returns == "void":
        body += ctext.packed(f"    {item.name}", args, ";") + "\n"
    else:
        body += ctext.packed(f"    return {item.name}", args, ";") + "\n"
    return _wrapped(signature + "\n{\n" + body + "}\n\n", condition(item))


def table_text(api: Api) -> str:
    items = tabled(api)
    thunks = "".join(_thunk(n, i) for n, i in enumerate(items))
    members = "".join(
        _wrapped(f"    .{i.stem} = VMAF_COMPAT_THUNK({n}),\n", condition(i))
        for n, i in enumerate(items)
    )
    side = (
        "#if VMAF_COMPAT_SIDE_NEW\n"
        "#define VMAF_COMPAT_THUNK(n) new_##n\n"
        "#define VMAF_COMPAT_CALLS vmaf_compat_calls_new\n"
        "#define VMAF_COMPAT_TABLE vmaf_compat_new\n"
        "#define VMAF_COMPAT_ENTRIES_DEFINE\n"
        "#else\n"
        "#define VMAF_COMPAT_THUNK(n) old_##n\n"
        "#define VMAF_COMPAT_CALLS vmaf_compat_calls_old\n"
        "#define VMAF_COMPAT_TABLE vmaf_compat_old\n"
        "#endif\n\n"
    )
    return (
        ctext.licence_block()
        + "\n"
        + ctext.banner_comment()
        + "\n/* Compiled twice (core/test/meson.build): VMAF_COMPAT_SIDE_NEW=0 with the\n"
        + " * engine's names (the old libvmaf bodies), =1 with VMAF_PUBLIC_NAMES (the\n"
        + " * compat library). */\n\n"
        + side
        + '#include "compat_conformance_gen.h"\n\n'
        + ctext.NULLPTR_BEGIN
        + "\n\n"
        + "unsigned VMAF_COMPAT_CALLS[VMAF_COMPAT_COUNT];\n\n"
        + thunks
        + "const VmafCompatApi VMAF_COMPAT_TABLE = {\n"
        + "    .count = VMAF_COMPAT_COUNT,\n"
        + members
        + "};\n\n"
        + ctext.NULLPTR_END
        + "\n"
    )
