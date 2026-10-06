# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Whole-definition checks (ADR-1852). Every refusal names the entry.

Unknown types, a field before `struct_size`, a by-value struct cycle, a
sized struct inside an unsized one, an entry without a header or `since`, a
`since` newer than the ABI, a deprecation without a replacement, a duplicate
symbol or constant value, a misplaced error parameter, a callback without
`void *user` and a compat field map that misses a field all stop generation.
"""

from __future__ import annotations

from collections.abc import Iterator

from . import format_table, graph, typesys
from .model import (
    HEADER_GROUPS,
    PASS_MODES,
    Api,
    Callback,
    Compat,
    DefinitionError,
    Deprecation,
    Field,
    Function,
    Param,
    Struct,
    version_text,
)

RESERVED_STEMS = ("index", "reference")  # hand-written / generated docs index pages
FLAG_WIDTH = {"u32": 32, "u64": 64}


def _unique(names: list[str], what: str) -> None:
    seen: set[str] = set()
    for name in names:
        if name in seen:
            raise DefinitionError(f"duplicate {what}: {name}")
        seen.add(name)


def constants(api: Api) -> dict[str, int]:
    """Every named constant (status codes, enum values, flag bits as masks)."""
    values = {s.name: s.value for s in api.statuses}
    for enum in api.enums:
        values.update({v.name: v.value for v in enum.values})
    for flags in api.flags:
        values.update({b.name: 1 << b.bit for b in flags.bits})
    return values


def _check_headers(api: Api) -> None:
    _unique([h.path for h in api.headers], "header")
    _unique([h.guard for h in api.headers], "header guard")
    for header in api.headers:
        if header.group not in HEADER_GROUPS:
            raise DefinitionError(f"headers[{header.path}]: group is one of {HEADER_GROUPS}")
        if header.stem in RESERVED_STEMS:
            raise DefinitionError(f"headers[{header.path}]: {header.stem} is a docs page name")
    if [h.group for h in api.headers].count("umbrella") != 1:
        raise DefinitionError("exactly one [[headers]] entry has group = umbrella")
    for role, path in (("base_header", api.base_header), ("version_header", api.version_header)):
        if api.header(path).group != "core":
            raise DefinitionError(f"api.{role} {path} must be a core header")


def entries(api: Api) -> Iterator[tuple[str, str, tuple[int, int], Deprecation | None]]:
    """(kind, name, since, deprecated) of every top-level entry."""
    for kind, items in (
        ("status", api.statuses),
        ("enum", api.enums),
        ("flags", api.flags),
        ("handle", api.handles),
        ("callback", api.callbacks),
        ("struct", api.structs),
        ("function", api.functions),
    ):
        for item in items:
            yield kind, item.name, item.since, item.deprecated


def members(api: Api) -> Iterator[tuple[str, tuple[int, int], tuple[int, int], Deprecation | None]]:
    """(name, since, parent since, deprecated) of every enum value, flag bit and field."""
    for enum in api.enums:
        for value in enum.values:
            yield value.name, value.since, enum.since, value.deprecated
    for flags in api.flags:
        for bit in flags.bits:
            yield bit.name, bit.since, flags.since, bit.deprecated
    for item in api.structs:
        for fld in item.fields:
            yield f"{item.name}.{fld.name}", fld.since, item.since, fld.deprecated


def _known_symbols(api: Api) -> set[str]:
    names = {f.name for f in api.functions} | set(constants(api))
    for items in (api.handles, api.structs, api.enums, api.callbacks, api.flags):
        names |= {item.name for item in items}
    names |= {f"{s.name}.{f.name}" for s in api.structs for f in s.fields}
    names |= {o.name for g in api.option_groups for o in g.options}
    return names


def _check_deprecation(
    api: Api, name: str, since: tuple[int, int], dep: Deprecation, known: set[str]
) -> None:
    where = f"{name}.deprecated"
    if not since <= dep.since <= api.abi_minor_node:
        raise DefinitionError(
            f"{where}: since {version_text(dep.since)} is outside the entry's life"
        )
    if dep.removal <= dep.since:
        raise DefinitionError(f"{where}: removal {version_text(dep.removal)} must follow since")
    if dep.replacement not in known:
        raise DefinitionError(f"{where}: replacement {dep.replacement!r} is not a declared name")


def _check_versions(api: Api) -> None:
    known = _known_symbols(api)
    newest = api.abi_minor_node
    for _, name, since, dep in entries(api):
        if since > newest:
            raise DefinitionError(
                f"{name}: since {version_text(since)} is newer than ABI {version_text(api.abi_version)}"
            )
        if dep is not None:
            _check_deprecation(api, name, since, dep, known)
    for name, since, parent, dep in members(api):
        if not parent <= since <= newest:
            raise DefinitionError(
                f"{name}: since {version_text(since)} is outside {version_text(parent)}..{version_text(newest)}"
            )
        if dep is not None:
            _check_deprecation(api, name, since, dep, known)
    for group in api.option_groups:
        for option in group.options:
            if option.deprecated is not None:
                _check_deprecation(
                    api, f"{group.name}.{option.name}", option.since, option.deprecated, known
                )


def _check_field_kind(api: Api, item: Struct, fld: Field) -> None:
    where = f"structs[{item.name}].{fld.name}"
    kind = typesys.kind(api, fld.type)
    if kind == "scalar" and fld.type not in typesys.FIELD_SCALARS:
        raise DefinitionError(f"{where}: {fld.type} is not a field type")
    if kind not in ("scalar", "struct", "handle", "callback"):
        raise DefinitionError(
            f"{where}: fields are fixed-width scalars, structs, handles or callbacks"
        )
    if not 0 <= fld.count <= typesys.MAX_ARRAY:
        raise DefinitionError(f"{where}: count is 1..{typesys.MAX_ARRAY} for an array")
    if fld.const and kind != "handle":
        raise DefinitionError(f"{where}: `const` applies to handle fields")
    if kind == "struct" and api.struct(fld.type).sized and not item.sized:
        raise DefinitionError(f"{where}: an unsized struct cannot embed sized {fld.type}")


def _check_annotations(api: Api, item: Struct, fld: Field) -> None:
    where = f"structs[{item.name}].{fld.name}"
    if fld.enum and (fld.type != "u32" or not api.is_enum(fld.enum)):
        raise DefinitionError(f"{where}: `enum` needs type u32 and a declared enum")
    if fld.flags:
        flags = api.flag_set(fld.flags)
        if flags is None or flags.type != fld.type:
            raise DefinitionError(f"{where}: `flags` needs a declared flag set of type {fld.type}")


def _check_structs(api: Api) -> None:
    for item in api.structs:
        for fld in item.fields:
            _check_field_kind(api, item, fld)
            _check_annotations(api, item, fld)
    names = [s.name for s in api.structs]
    by_value = {s.name: {f.type for f in s.fields if api.is_struct(f.type)} for s in api.structs}
    graph.order(names, by_value, "by-value struct")


def _check_param(api: Api, owner: str, param: Param) -> None:
    where = f"{owner}.{param.name}"
    if param.mode not in PASS_MODES:
        raise DefinitionError(f"{where}: unknown pass mode {param.mode!r}")
    kind = typesys.kind(api, param.type)
    allowed = {
        "in": ("scalar", "struct", "handle", "foreign", "callback"),
        "error": ("handle",),
        "out_handle": ("handle",),
        "in_const": ("handle", "struct"),
        "out": ("scalar", "struct"),
    }[param.mode]
    if kind not in allowed:
        raise DefinitionError(f"{where}: pass {param.mode} takes a {' or '.join(allowed)}")
    if param.mode == "error" and param.type != "VmafxError":
        raise DefinitionError(f"{where}: the error parameter is a VmafxError")


def _check_function(api: Api, fn: Function) -> None:
    where = f"functions[{fn.name}]"
    if fn.returns != "void":
        typesys.kind(api, fn.returns)
    for param in fn.params:
        _check_param(api, where, param)
    has_error = any(p.mode == "error" for p in fn.params)
    if has_error and (fn.returns != "status" or fn.params[-1].mode != "error"):
        raise DefinitionError(f"{where}: the error out-parameter is last and the result a status")


def _check_callback(api: Api, item: Callback) -> None:
    where = f"callbacks[{item.name}]"
    if item.returns != "void" and typesys.kind(api, item.returns) != "scalar":
        raise DefinitionError(f"{where}: a callback returns void or a scalar")
    for param in item.params:
        _check_param(api, where, param)
        if param.mode not in ("in", "in_const"):
            raise DefinitionError(f"{where}.{param.name}: callback parameters are inputs")
    if not item.params or (item.params[-1].name, item.params[-1].type) != ("user", "ptr"):
        raise DefinitionError(f"{where}: the last parameter is `user` of type ptr (void *user)")


def _check_flags(api: Api) -> None:
    for flags in api.flags:
        width = FLAG_WIDTH.get(flags.type)
        if width is None:
            raise DefinitionError(f"flags[{flags.name}]: type is u32 or u64")
        _unique([str(b.bit) for b in flags.bits], f"bit of {flags.name}")
        for bit in flags.bits:
            if not 0 <= bit.bit < width:
                raise DefinitionError(f"flags[{flags.name}].{bit.name}: bit outside 0..{width - 1}")


def _check_compat(api: Api, item: Compat) -> None:
    where = f"compat[{item.name}]"
    if item.kind not in ("shim", "glue"):
        raise DefinitionError(f"{where}: generated kinds are shim and glue")
    api.function(item.target)
    build = item.spec.get("build")
    if not isinstance(build, dict):
        return
    target = api.struct(str(build["type"]))
    wanted = {f.name for f in target.fields if f.name != "struct_size"}
    given = set(build.get("from", {}))
    if wanted != given:
        missing, extra = sorted(wanted - given), sorted(given - wanted)
        raise DefinitionError(
            f"{where}: field map of {target.name} missing {missing} extra {extra}"
        )


def _check_placement(api: Api) -> None:
    """Every declaration names a declared, non-umbrella header."""
    paths = {h.path: h.group for h in api.headers}
    for items in (api.enums, api.flags, api.handles, api.callbacks, api.structs, api.functions):
        for item in items:
            if item.header not in paths:
                raise DefinitionError(f"{item.name}: header {item.header} is not declared")
            if paths[item.header] == "umbrella":
                raise DefinitionError(f"{item.name}: the umbrella header only includes the others")
    for handle in api.handles:
        if handle.release not in {f.name for f in api.functions}:
            raise DefinitionError(
                f"handles[{handle.name}]: release {handle.release} is not declared"
            )


def _check_names(api: Api) -> None:
    _unique([s.name for s in api.statuses], "status")
    _unique([str(s.value) for s in api.statuses], "status value")
    _unique(list(constants(api)), "constant")
    _unique([f.name for f in api.functions] + [c.name for c in api.compats], "function")
    types = [h.name for h in api.handles] + [s.name for s in api.structs]
    types += (
        [e.name for e in api.enums] + [c.name for c in api.callbacks] + [f.name for f in api.flags]
    )
    _unique(types, "type")
    for enum in api.enums:
        _unique([str(v.value) for v in enum.values], f"value of {enum.name}")
    for item in api.structs:
        _unique([f.name for f in item.fields], f"field of {item.name}")


def validate(api: Api) -> None:
    _check_headers(api)
    _check_names(api)
    _check_placement(api)
    _check_flags(api)
    _check_structs(api)
    for fn in api.functions:
        _check_function(api, fn)
    for callback in api.callbacks:
        _check_callback(api, callback)
    for compat in api.compats:
        _check_compat(api, compat)
    _check_versions(api)
    format_table.check(api)
