# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""libvmaf compatibility layer (ADR-1852 design section 2.11, decision D3).

`compat_source()` writes core/src/compat/libvmaf/libvmaf_gen.c: each `shim` /
`glue` entry of `[[compat]]` becomes the definition of a function libvmaf's
headers already declare, written on top of the vmafx_ function named by
`target`. The shim keeps libvmaf's contract: the same NULL checks, the same
output clearing and the engine's own negative errno when the failure came from
the engine (compat_errno.h). `manual` entries live in hand-written files of
the same directory; `engine` entries are not compat functions at all (they
stay in the engine while their backend is built).

`engine_names()` writes core/src/vmafx/engine_names_gen.h, which every engine
translation unit is compiled with (`-include`): it renames the engine's own
definition of each compat function to `vmaf_engine_<stem>`, so the engine
library defines no libvmaf name and a static link of both libraries cannot
resolve a libvmaf call to the engine (or recurse through a shim).

Both outputs read the build switches of meson (`VMAFX_ENGINE_EXPORTS_<B>`: the
engine keeps and exports backend B's libvmaf functions; `VMAFX_BUILD_<F>`:
feature F is built).
"""

from __future__ import annotations

from . import ctext
from .model import COMPAT_BACKENDS, COMPAT_FEATURES, Api, Compat

PATH = "core/src/compat/libvmaf/libvmaf_gen.c"
ENGINE_NAMES_PATH = "core/src/vmafx/engine_names_gen.h"


def engine_switch(backend: str) -> str:
    return f"VMAFX_ENGINE_EXPORTS_{backend.upper()}"


def feature_switch(feature: str) -> str:
    return f"VMAFX_BUILD_{feature.upper()}"


def condition(item: Compat) -> str:
    """Preprocessor condition under which the compat layer defines `item`; "" = always."""
    parts = []
    if item.engine_with:
        parts.append(f"!{engine_switch(item.engine_with)}")
    if item.when:
        parts.append(feature_switch(item.when))
    return " && ".join(parts)


def _wrapped(text: str, cond: str) -> str:
    if not cond:
        return text
    return f"#if {cond}\n{text}#endif /* {cond} */\n"


def _signature(item: Compat) -> str:
    head = ctext.pointer_join(item.returns, item.name)
    params = [ctext.pointer_join(c_type, name) for name, c_type in item.params] or ["void"]
    return ctext.packed(head, params, "")


def _guard(condition_text: str, result: str) -> str:
    return f"    if ({condition_text}) {{\n        return {result};\n    }}\n"


def _prologue(item: Compat) -> str:
    spec = item.spec
    out = [_guard(f"!{name}", "-EINVAL") for name in spec.get("null_checks", [])]
    if "clear" in spec:
        out.append(f"    {spec['clear']}\n")
    if "context" in spec:
        out.append(f"    VmafxContext *context = vmafx_context_from_libvmaf({spec['context']});\n")
        out.append(_guard("!context", "-EINVAL"))
    return "".join(out)


def _locals(api: Api, item: Compat) -> str:
    spec = item.spec
    out = []
    build = spec.get("build")
    if build:
        target = api.struct(build["type"])
        out.append(f"    {target.name} {build['var']} = {target.init_macro};\n")
        for field in target.fields[1:]:
            out.append(f"    {build['var']}.{field.name} = {build['from'][field.name]};\n")
    handle = spec.get("out_handle")
    if handle:
        out.append(f"    {handle['type']} *{handle['var']} = NULL;\n")
    record = spec.get("out_struct")
    if record:
        init = api.struct(record["type"]).init_macro
        out.append(f"    {record['type']} {record['var']} = {init};\n")
    out.append("    VmafxError *error = NULL;\n")
    return "".join(out)


def _status_body(api: Api, item: Compat) -> str:
    call = ctext.assignment("    const VmafxStatus status", item.target, item.spec["args"])
    store = item.spec.get("store")
    handle = item.spec.get("out_handle")
    # Power of 10 rule 5: a successful create hands back a handle, and `post`
    # names what a successful call guarantees of its outputs.
    check = f"    assert({handle['var']} != NULL);\n" if handle else ""
    check += "".join(f"    assert({expr});\n" for expr in item.spec.get("post", []))
    return (
        _prologue(item)
        + _locals(api, item)
        + call
        + "\n"
        + _guard("status != VMAFX_OK", "compat_errno(status, error)")
        + check
        + (f"    {store}\n" if store else "")
        + "    return 0;\n"
    )


def _function(api: Api, item: Compat) -> str:
    doc = f"/* {item.header}: {item.name}() on {item.target}() ({item.kind}). */\n"
    if "body" in item.spec:
        body = "".join(f"    {line}\n" for line in item.spec["body"])
    elif "return_expr" in item.spec:
        body = f"    return {item.spec['return_expr']};\n"
    else:
        body = _status_body(api, item)
    return _wrapped(doc + _signature(item) + "\n{\n" + body + "}\n", condition(item))


def _api_headers(api: Api, items: list[Compat]) -> list[str]:
    """The umbrella plus every optional header declaring a function the shims call."""
    declared = {fn.name: fn.header for fn in api.functions}
    called = {item.target for item in items} | {
        "vmafx_context_from_libvmaf",
        "vmafx_context_libvmaf_handle",
    }
    called |= {name for item in items for name in item.spec.get("uses", [])}
    used = {declared[name] for name in called if name in declared}
    optional = [h.path for h in api.headers if h.group == "optional" and h.path in used]
    return [api.umbrella.path, *optional]


def compat_source(api: Api) -> str:
    items = [item for item in api.compats if item.generated]
    headers = sorted({c.header for c in items})
    includes = ["#include <assert.h>", "#include <errno.h>", "#include <stddef.h>"]
    includes += ["#include <stdint.h>", ""]
    includes += [f'#include "{h}"' for h in headers]
    includes += ['#include "compat_errno.h"']
    includes += [f'#include "{path}"' for path in _api_headers(api, items)]
    functions = "\n".join(_function(api, item) for item in items)
    return (
        ctext.licence_block()
        + "\n"
        + ctext.banner_comment()
        + "\n"
        + "\n".join(includes)
        + "\n\n"
        + ctext.NULLPTR_BEGIN
        + "\n\n"
        + (functions + "\n" if functions else "")
        + ctext.NULLPTR_END
        + "\n"
    )


def _switch_checks() -> str:
    names = [engine_switch(b) for b in COMPAT_BACKENDS] + [
        feature_switch(f) for f in COMPAT_FEATURES
    ]
    note = "/* core/src/meson.build sets each switch (vmaf_engine_name_args). */\n"
    return note + "".join(f'#ifndef {name}\n#error "{name} is not set"\n#endif\n' for name in names)


def _rename_block(items: list[Compat]) -> str:
    out = []
    for item in items:
        line = ctext.object_macro(item.name, f"vmaf_engine_{item.stem}")
        out.append(_wrapped(line, condition(item)))
    return "".join(out)


def engine_names(api: Api) -> str:
    """Forced include of every engine translation unit (core/src/meson.build)."""
    items = [item for item in api.compats if item.kind != "engine"]
    renames = _rename_block(items)
    intro = (
        "/*\n"
        " * Every engine translation unit is compiled with this header first. A libvmaf\n"
        " * function the compat library (libvmaf.so.3) defines is named\n"
        " * vmaf_engine_<stem> inside the engine (libvmafx.so.1), so the engine\n"
        " * defines no libvmaf name: the public headers declare, the engine sources\n"
        " * define and call, the renamed functions. A libvmaf function the engine keeps\n"
        " * while its backend is built (an `engine` exception) keeps its name.\n"
        " */\n"
    )
    guard = "VMAFX_ENGINE_NAMES_GEN_H"
    return (
        ctext.licence_block()
        + "\n"
        + ctext.banner_comment()
        + "\n"
        + intro
        + "\n"
        + f"#ifndef {guard}\n#define {guard}\n\n"
        + _switch_checks()
        + "\n/* Compat library, tools and black-box tests keep the libvmaf names. */\n"
        + "#ifndef VMAF_PUBLIC_NAMES\n"
        + ("\n" + renames if renames else "")
        + "\n#endif /* !VMAF_PUBLIC_NAMES */\n"
        + f"\n#endif /* {guard} */\n"
    )
