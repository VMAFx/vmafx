# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Public C headers (core/include/vmafx/) and the status tables (core/src/vmafx/)."""

from __future__ import annotations

from . import ctext
from .headers import HeaderPlan
from .model import (
    Api,
    Callback,
    DefinitionError,
    Deprecation,
    Enum,
    Field,
    Flags,
    Function,
    Status,
    Struct,
    upper_snake,
    version_text,
)

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
DEPRECATED_DEFINITION = """/** Marks a deprecated function; define VMAFX_NO_DEPRECATION_WARNINGS to silence it. */
#ifndef VMAFX_DEPRECATED
#if defined(VMAFX_NO_DEPRECATION_WARNINGS)
#define VMAFX_DEPRECATED(message)
#elif defined(_MSC_VER)
#define VMAFX_DEPRECATED(message) __declspec(deprecated(message))
#elif defined(__GNUC__) || defined(__clang__)
#define VMAFX_DEPRECATED(message) __attribute__((deprecated(message)))
#else
#define VMAFX_DEPRECATED(message)
#endif
#endif
"""
ESCAPE_COLUMN = ctext.COLUMNS - 1  # clang-format aligns `\` to the column limit


def _tags(
    since: tuple[int, int], dep: Deprecation | None, parent: tuple[int, int] | None = None
) -> tuple[str, ...]:
    """`@since` (when it differs from the parent's) and `@deprecated` lines."""
    tags = [] if since == parent else [f"@since {version_text(since)}"]
    if dep is not None:
        tags.append(f"@deprecated {ctext.deprecation_note(dep)}")
    return tuple(tags)


def _status_block(api: Api) -> str:
    first = api.statuses[0].since
    out = ["/** Result of every fallible call: 0 success, > 0 informational, < 0 error. */\n"]
    out.append("typedef int32_t VmafxStatus;\n\n")
    out.append(ctext.doc_block("Status codes; stable on every platform.", extra=_tags(first, None)))
    out.append("enum VmafxStatusCode {\n")
    for status in api.statuses:
        out.append(
            ctext.doc_block(status.doc, "    ", _tags(status.since, status.deprecated, first))
        )
        out.append(f"    {status.name} = {status.value},\n")
    out.append("};\n")
    return "".join(out)


def _enum_block(enum: Enum) -> str:
    out = [ctext.doc_block(enum.doc, extra=_tags(enum.since, enum.deprecated))]
    out.append(f"typedef enum {enum.name} {{\n")
    for value in enum.values:
        tags = _tags(value.since, value.deprecated, enum.since)
        if value.doc or tags:
            out.append(ctext.doc_block(value.doc, "    ", tags))
        out.append(f"    {value.name} = {value.value},\n")
    out.append(f"}} {enum.name};\n")
    return "".join(out)


def _flags_block(flags: Flags) -> str:
    suffix = "UINT32_C(1)" if flags.type == "u32" else "UINT64_C(1)"
    text = f"{flags.name}: {flags.doc} Bits of a {flags.type} field."
    block = ctext.doc_block(text, extra=_tags(flags.since, flags.deprecated))
    out = ["/*" + block[3:]]  # a plain comment: each bit carries its own doc block
    for bit in flags.bits:
        out.append(ctext.doc_block(bit.doc, extra=_tags(bit.since, bit.deprecated, flags.since)))
        out.append(f"#define {bit.name} ({suffix} << {bit.bit}U)\n")
    return "".join(out)


def _field_block(api: Api, item: Struct, fld: Field) -> str:
    doc = fld.doc + (f" Values: {fld.enum}." if fld.enum else "")
    doc += f" Bits: {fld.flags}." if fld.flags else ""
    tags = _tags(fld.since, fld.deprecated, item.since)
    return ctext.doc_block(doc.strip(), "    ", tags) + "    " + ctext.field_decl(api, fld) + "\n"


def init_macro(api: Api, item: Struct) -> str:
    """`#define T_INIT {...}`: sets struct_size, also of every nested sized struct."""
    items = [f".struct_size = sizeof({item.name})"]
    for fld in item.fields:
        if not (api.is_struct(fld.type) and api.struct(fld.type).sized):
            continue
        nested = api.struct(fld.type).init_macro
        value = "{" + ", ".join([nested] * fld.count) + "}" if fld.count else nested
        items.append(f".{fld.name} = {value}")
    single = f"#define {item.init_macro} {{" + ", ".join(items) + "}"
    if len(single) <= ctext.COLUMNS:
        return single + "\n"
    lines = [f"#define {item.init_macro}", "    {" + items[0] + ","]
    lines += [f"     {text}," for text in items[1:-1]] + [f"     {items[-1]}}}"]
    if any(len(line) > ESCAPE_COLUMN - 1 for line in lines):
        raise DefinitionError(
            f"{item.init_macro}: an initialiser line exceeds {ctext.COLUMNS} columns"
        )
    escaped = [line.ljust(ESCAPE_COLUMN) + "\\" for line in lines[:-1]]
    # clang-format packs a long initialiser differently depending on its items;
    # the fence keeps this one layout, so the drift check stays byte-exact.
    return (
        "\n".join(["/* clang-format off */", *escaped, lines[-1], "/* clang-format on */"]) + "\n"
    )


def _struct_block(api: Api, item: Struct) -> str:
    out = [ctext.doc_block(item.doc, extra=_tags(item.since, item.deprecated))]
    out.append(f"struct {item.name} {{\n")
    out.extend(_field_block(api, item, fld) for fld in item.fields)
    out.append("};\n")
    if item.sized:
        out.append("\n/** Initialiser that sets `struct_size`; every other field is zero. */\n")
        out.append(init_macro(api, item))
    return "".join(out)


def _callback_block(api: Api, item: Callback) -> str:
    doc = ctext.doc_block(item.doc, extra=_tags(item.since, item.deprecated))
    return doc + ctext.callback_typedef(api, item) + "\n"


def _function_block(api: Api, fn: Function) -> str:
    doc = ctext.doc_block(fn.doc, extra=_tags(fn.since, fn.deprecated))
    return doc + ctext.declaration(api, fn, api.export_macro) + "\n"


def _version_block(api: Api) -> str:
    major, minor, patch = api.abi_version
    prefix = upper_snake(api.name.capitalize())
    return (
        "/** ABI version this header describes (ADR-1852). */\n"
        f"#define {prefix}_ABI_VERSION_MAJOR {major}\n"
        f"#define {prefix}_ABI_VERSION_MINOR {minor}\n"
        f"#define {prefix}_ABI_VERSION_PATCH {patch}\n"
    )


def _fixed_blocks(api: Api, plan: HeaderPlan) -> list[str]:
    path = plan.header.path
    parts: list[str] = []
    if path == api.base_header:
        parts += [EXPORT_DEFINITION.format(macro=api.export_macro), DEPRECATED_DEFINITION]
        parts.append(_status_block(api))
    if path == api.version_header:
        parts.append(_version_block(api))
    return parts


def _body(api: Api, plan: HeaderPlan) -> list[str]:
    parts = _fixed_blocks(api, plan)
    parts += [_enum_block(enum) for enum in plan.enums]
    parts += [_flags_block(flags) for flags in plan.flags]
    for handle in plan.handles:
        doc = ctext.doc_block(handle.doc, extra=_tags(handle.since, handle.deprecated))
        parts.append(doc + f"typedef struct {handle.name} {handle.name};\n")
    if plan.structs:
        parts.append("".join(f"typedef struct {s.name} {s.name};\n" for s in plan.structs))
    parts += [_callback_block(api, item) for item in plan.callbacks]
    parts += [_struct_block(api, item) for item in plan.structs]
    parts += [_function_block(api, fn) for fn in plan.functions]
    return parts


def _includes(plan: HeaderPlan) -> list[str]:
    includes = (
        [] if plan.header.group == "umbrella" else ["#include <stddef.h>", "#include <stdint.h>"]
    )
    includes += [f"#include <{path}>" for path in plan.includes]
    includes += [f"#include <{path}>" for path in plan.header.includes]
    return includes


def _wrapped(body: list[str]) -> str:
    """Declarations inside `extern "C"`; the umbrella holds none and gets no block."""
    if not body:
        return ""
    return (
        '#ifdef __cplusplus\nextern "C" {\n#endif\n\n'
        + "\n".join(body)
        + "\n#ifdef __cplusplus\n}\n#endif\n\n"
    )


def header_text(api: Api, plan: HeaderPlan) -> str:
    header = plan.header
    body = _body(api, plan)
    if header.group != "umbrella" and not body:
        body = ["/* No declarations yet: later RC4 work packages fill this header. */\n"]
    return (
        ctext.licence_block()
        + "\n"
        + ctext.banner_comment()
        + "\n"
        + ctext.doc_block(header.brief)
        + f"\n#ifndef {header.guard}\n#define {header.guard}\n\n"
        + "\n".join(_includes(plan))
        + "\n\n"
        + _wrapped(body)
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


def _status_header_path(api: Api) -> str:
    return api.function("vmafx_status_name").header


def status_source(api: Api) -> str:
    return (
        ctext.licence_block()
        + "\n"
        + ctext.banner_comment()
        + "\n"
        + '#include <errno.h>\n#include <stddef.h>\n\n#include "status_gen.h"\n'
        + f'#include "{_status_header_path(api)}"\n\n'
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
        + f'#include "{api.base_header}"\n\n'
        + "/** Negative errno libvmaf returns for a status without an engine errno. */\n"
        + "int vmafx_status_to_errno(VmafxStatus status);\n\n"
        + "/** Status for a negative errno the engine returned. */\n"
        + "VmafxStatus vmafx_status_from_errno(int negative_errno);\n\n"
        + "#endif /* VMAFX_STATUS_GEN_H */\n"
    )
