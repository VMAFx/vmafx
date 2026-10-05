# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Public C headers and the status tables (core/include/vmafx/, core/src/vmafx/)."""

from __future__ import annotations

from . import ctext
from .model import Api, Enum, Header, Status, Struct, upper_snake

EXPORT_DEFINITION = """#ifndef {macro}
#if defined(_MSC_VER)
#define {macro} __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define {macro} __attribute__((visibility("default")))
#else
#define {macro}
#endif
#endif
"""


def _status_block(api: Api) -> str:
    out = ["/** Result of every fallible call: 0 success, > 0 informational, < 0 error. */\n"]
    out.append("typedef int32_t VmafxStatus;\n\n")
    out.append("/** Status codes; stable on every platform. */\n")
    out.append("enum VmafxStatusCode {\n")
    for status in api.statuses:
        out.append(ctext.doc_block(status.doc, indent="    "))
        out.append(f"    {status.name} = {status.value},\n")
    out.append("};\n")
    return "".join(out)


def _enum_block(enum: Enum) -> str:
    out = [ctext.doc_block(enum.doc), f"typedef enum {enum.name} {{\n"]
    out.extend(f"    {value.name} = {value.value},\n" for value in enum.values)
    out.append(f"}} {enum.name};\n")
    return "".join(out)


def _struct_block(item: Struct) -> str:
    out = [ctext.doc_block(item.doc), f"typedef struct {item.name} {{\n"]
    for field in item.fields:
        doc = field.doc + (f" Values: {field.enum}." if field.enum else "")
        out.append(ctext.doc_block(doc, indent="    "))
        out.append("    " + ctext.pointer_join(ctext.field_type(field), field.name) + ";\n")
    out.append(f"}} {item.name};\n")
    if item.sized:
        out.append("\n/** Initialiser that sets `struct_size`; every other field is zero. */\n")
        out.append(f"#define {item.init_macro} {{.struct_size = sizeof({item.name})}}\n")
    return "".join(out)


def _version_block(api: Api) -> str:
    major, minor, patch = api.abi_version
    prefix = upper_snake(api.name.capitalize())
    return (
        "/** ABI version this header describes (ADR-1852). */\n"
        f"#define {prefix}_ABI_VERSION_MAJOR {major}\n"
        f"#define {prefix}_ABI_VERSION_MINOR {minor}\n"
        f"#define {prefix}_ABI_VERSION_PATCH {patch}\n"
    )


def _types_block(api: Api) -> list[str]:
    parts = [_version_block(api), _status_block(api)]
    parts.extend(_enum_block(enum) for enum in api.enums)
    for handle in api.handles:
        parts.append(ctext.doc_block(handle.doc) + f"typedef struct {handle.name} {handle.name};\n")
    parts.extend(_struct_block(item) for item in api.structs)
    return parts


def _function_blocks(api: Api, header: Header) -> list[str]:
    parts = []
    for fn in api.functions:
        if fn.header != header.path:
            continue
        doc = ctext.doc_block(fn.doc, extra=(f"@since {fn.since}",))
        parts.append(doc + ctext.declaration(api, fn, api.export_macro) + "\n")
    return parts


def header_text(api: Api, header: Header) -> str:
    includes = ["#include <stddef.h>", "#include <stdint.h>"]
    includes += [f"#include <{path}>" for path in header.includes]
    body: list[str] = []
    if header is api.headers[0]:
        body.append(EXPORT_DEFINITION.format(macro=api.export_macro))
        body.extend(_types_block(api))
    body.extend(_function_blocks(api, header))
    return (
        ctext.licence_block()
        + "\n"
        + ctext.banner_comment()
        + "\n"
        + ctext.doc_block(header.brief)
        + f"\n#ifndef {header.guard}\n#define {header.guard}\n\n"
        + "\n".join(includes)
        + '\n\n#ifdef __cplusplus\nextern "C" {\n#endif\n\n'
        + "\n".join(body)
        + "\n#ifdef __cplusplus\n}\n#endif\n\n"
        + f"#endif /* {header.guard} */\n"
    )


def _errno_expr(status: Status) -> str:
    return "0" if status.errno == "0" else f"-{status.errno}"


STATUS_FUNCTIONS = """const char *vmafx_status_name(VmafxStatus status)
{
    for (size_t i = 0; i < N_STATUS_ROWS; i++) {
        if (status_rows[i].status == status) {
            return status_rows[i].name;
        }
    }
    return "VMAFX_UNKNOWN_STATUS";
}

int vmafx_status_to_errno(VmafxStatus status)
{
    for (size_t i = 0; i < N_STATUS_ROWS; i++) {
        if (status_rows[i].status == status) {
            return status_rows[i].negative_errno;
        }
    }
    return -EIO;
}

VmafxStatus vmafx_status_from_errno(int negative_errno)
{
    for (size_t i = 0; i < N_STATUS_ROWS; i++) {
        if (status_rows[i].reverse && status_rows[i].negative_errno == negative_errno) {
            return status_rows[i].status;
        }
    }
    return VMAFX_E_INTERNAL;
}
"""


def _status_rows(api: Api) -> str:
    """One row per status; `reverse` marks the status an errno maps back to (first wins)."""
    seen: set[str] = set()
    rows = []
    for status in api.statuses:
        reverse = status.reverse and status.errno not in seen
        if reverse:
            seen.add(status.errno)
        flag = "1" if reverse else "0"
        rows.append(f'    {{{status.name}, "{status.name}", {_errno_expr(status)}, {flag}}},\n')
    return (
        "/* status, name, negative errno for libvmaf, maps back from that errno */\n"
        "static const struct {\n    VmafxStatus status;\n    const char *name;\n"
        "    int negative_errno;\n    int reverse;\n} status_rows[] = {\n"
        + "".join(rows)
        + "};\n\n#define N_STATUS_ROWS (sizeof(status_rows) / sizeof(status_rows[0]))\n"
    )


def status_source(api: Api) -> str:
    return (
        ctext.licence_block()
        + "\n"
        + ctext.banner_comment()
        + "\n"
        + '#include <errno.h>\n#include <stddef.h>\n\n#include "status_gen.h"\n'
        + '#include "vmafx/vmafx.h"\n\n'
        + _status_rows(api)
        + "\n"
        + STATUS_FUNCTIONS
    )


def status_header(api: Api) -> str:
    return (
        ctext.licence_block()
        + "\n"
        + ctext.banner_comment()
        + "\n"
        + "/* Internal status <-> errno maps; not exported (the library builds with\n"
        + " * -fvisibility=hidden and these carry no export attribute). */\n\n"
        + "#ifndef VMAFX_STATUS_GEN_H\n#define VMAFX_STATUS_GEN_H\n\n"
        + '#include "vmafx/vmafx.h"\n\n'
        + "/** Negative errno libvmaf returns for a status without an engine errno. */\n"
        + "int vmafx_status_to_errno(VmafxStatus status);\n\n"
        + "/** Status for a negative errno the engine returned. */\n"
        + "VmafxStatus vmafx_status_from_errno(int negative_errno);\n\n"
        + "#endif /* VMAFX_STATUS_GEN_H */\n"
    )
