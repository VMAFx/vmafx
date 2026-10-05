# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""libvmaf compatibility shims (core/src/vmafx/compat_libvmaf_gen.c).

Each `[[compat]]` entry becomes the definition of a function libvmaf.h already
declares, written on top of the vmafx_ function named by `target`. The shim
keeps libvmaf's contract: the same NULL checks, the same output clearing and
the engine's own negative errno when the failure came from the engine.
"""

from __future__ import annotations

from . import ctext
from .model import Api, Compat

HELPER_CALLS = (
    "vmafx_context_from_libvmaf",
    "vmafx_context_libvmaf_handle",
    "vmafx_error_errno",
    "vmafx_error_free",
)
HELPER = """/* The negative errno libvmaf returned for this failure: the engine's own code
 * when the failure came from the engine, else the status's errno. Releases
 * the error. */
static int compat_errno(VmafxStatus status, VmafxError *error)
{
    const int32_t engine_errno = vmafx_error_errno(error);
    vmafx_error_free(error);
    if (engine_errno != 0) {
        return engine_errno;
    }
    return vmafx_status_to_errno(status);
}
"""


def _signature(item: Compat) -> str:
    head = ctext.pointer_join(item.returns, item.name)
    params = [ctext.pointer_join(c_type, name) for name, c_type in item.params] or ["void"]
    return ctext.packed(head, params, "")


def _guard(condition: str, result: str) -> str:
    return f"    if ({condition}) {{\n        return {result};\n    }}\n"


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
    # Power of 10 rule 5: a successful create hands back a handle.
    check = f"    assert({handle['var']} != NULL);\n" if handle else ""
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
    if "return_expr" in item.spec:
        body = f"    return {item.spec['return_expr']};\n"
    else:
        body = _status_body(api, item)
    return doc + _signature(item) + "\n{\n" + body + "}\n"


def _api_headers(api: Api) -> list[str]:
    """The umbrella plus every optional header declaring a function the shims call."""
    declared = {fn.name: fn.header for fn in api.functions}
    called = {item.target for item in api.compats} | set(HELPER_CALLS)
    used = {declared[name] for name in called if name in declared}
    optional = [h.path for h in api.headers if h.group == "optional" and h.path in used]
    return [api.umbrella.path, *optional]


def compat_source(api: Api) -> str:
    headers = sorted({c.header for c in api.compats})
    includes = ["#include <assert.h>", "#include <errno.h>", "#include <stddef.h>"]
    includes += ["#include <stdint.h>", ""]
    includes += [f'#include "{h}"' for h in headers]
    includes += ['#include "status_gen.h"']
    includes += [f'#include "{path}"' for path in _api_headers(api)]
    functions = "\n".join(_function(api, item) for item in api.compats)
    return (
        ctext.licence_block()
        + "\n"
        + ctext.banner_comment()
        + "\n"
        + "\n".join(includes)
        + "\n\n"
        + ctext.NULLPTR_BEGIN
        + "\n\n"
        + HELPER
        + "\n"
        + (functions + "\n" if functions else "")
        + ctext.NULLPTR_END
        + "\n"
    )
