# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""What a type name in the definition is, and how C spells a value of it.

Kinds: `scalar` (fixed-width numbers, `uptr`, `size`, `ptr`, `cstr`,
`status`), `struct` (embedded by value in a struct, passed by pointer),
`handle` (always a pointer), `callback` (a function pointer), `enum` (named
constants; fields and parameters carry them as `u32`), `foreign` (a type of
another header, `foreign:VmafContext`, always a pointer). `handle:T` as a
result is a pointer to handle T.
"""

from __future__ import annotations

from .model import SCALARS, Api, DefinitionError

FIELD_SCALARS = ("u32", "u64", "i32", "i64", "f32", "f64", "uptr", "size", "ptr", "cstr", "status")
MAX_ARRAY = 64  # a fixed array longer than this is a buffer, not a field


def kind(api: Api, name: str) -> str:
    if name in SCALARS:
        return "scalar"
    if name.startswith("foreign:"):
        return "foreign"
    if name.startswith("handle:"):
        return "handle_result"
    for test, label in (
        (api.is_struct, "struct"),
        (api.is_handle, "handle"),
        (api.is_callback, "callback"),
        (api.is_enum, "enum"),
    ):
        if test(name):
            return label
    raise DefinitionError(f"unknown type {name!r}")


def base_spelling(name: str) -> str:
    """The C type of a value of `name`, without the pointer a handle needs."""
    if name in SCALARS:
        return SCALARS[name]
    if name.startswith(("foreign:", "handle:")):
        return name.split(":", 1)[1]
    return name


def pointer_join(c_type: str, name: str) -> str:
    """`const char *` + `x` -> `const char *x`; `int` + `x` -> `int x`."""
    if c_type.endswith("*"):
        return c_type + name
    return f"{c_type} {name}"


def referenced_type(name: str) -> str:
    """Type name an entry refers to (`handle:VmafxContext` -> VmafxContext); "" for none."""
    if name in SCALARS:
        return "VmafxStatus" if name == "status" else ""
    if name.startswith("foreign:") or name == "void":
        return ""
    return name.split(":", 1)[1] if name.startswith("handle:") else name
