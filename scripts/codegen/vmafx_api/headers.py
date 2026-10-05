# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Which declarations go in which header, and what each header includes.

Every entry names its header. A header includes the headers that define the
types its declarations use (computed, never written by hand), so a consumer
can include any single header. The umbrella (`group = "umbrella"`) includes
every `core` header; `optional` headers (backend helpers, the libvmaf bridge)
are never included by it. An include cycle stops generation.
"""

from __future__ import annotations

from dataclasses import dataclass

from . import graph, typesys
from .model import Api, Callback, Enum, Flags, Function, Handle, Header, Struct


@dataclass(frozen=True)
class HeaderPlan:
    header: Header
    includes: tuple[str, ...]  # generated headers this one includes, in dependency order
    enums: tuple[Enum, ...]
    flags: tuple[Flags, ...]
    handles: tuple[Handle, ...]
    callbacks: tuple[Callback, ...]
    structs: tuple[Struct, ...]  # nested structs before the structs embedding them
    functions: tuple[Function, ...]

    @property
    def empty(self) -> bool:
        groups = (
            self.enums,
            self.flags,
            self.handles,
            self.callbacks,
            self.structs,
            self.functions,
        )
        return not any(groups)


def owners(api: Api) -> dict[str, str]:
    """Type name -> path of the header that defines it."""
    out = {"VmafxStatus": api.base_header}
    for items in (api.enums, api.flags, api.handles, api.callbacks, api.structs):
        out.update({item.name: item.header for item in items})
    return out


def _used_types(api: Api, path: str) -> set[str]:
    used: set[str] = set()
    for item in api.structs:
        if item.header == path:
            used |= {typesys.referenced_type(f.type) for f in item.fields}
    callables: list[Function | Callback] = [*api.functions, *api.callbacks]
    for fn in callables:
        if fn.header == path:
            used |= {typesys.referenced_type(p.type) for p in fn.params}
            used.add(typesys.referenced_type(fn.returns))
    return used - {""}


def _needs(api: Api, path: str) -> set[str]:
    owner = owners(api)
    needed = {owner[name] for name in _used_types(api, path) if name in owner}
    declares = any(fn.header == path for fn in api.functions)
    if path == api.version_header or declares:
        needed.add(api.base_header)  # VMAFX_EXPORT / VMAFX_DEPRECATED
    needed.discard(path)
    return needed


def include_order(api: Api) -> dict[str, tuple[str, ...]]:
    """Header path -> generated headers it includes; refuses an include cycle."""
    paths = [h.path for h in api.headers if h.group != "umbrella"]
    needs = {path: _needs(api, path) for path in paths}
    ranked = graph.order(paths, needs, "header include")
    out = {path: tuple(p for p in ranked if p in needs[path]) for path in paths}
    core = [p for p in ranked if api.header(p).group == "core"]
    out[api.umbrella.path] = tuple(core)
    return out


def _struct_order(api: Api, path: str) -> tuple[Struct, ...]:
    local = {s.name: s for s in api.structs if s.header == path}
    needs = {name: {f.type for f in s.fields if f.type in local} for name, s in local.items()}
    return tuple(local[name] for name in graph.order(list(local), needs, "by-value struct"))


def plan(api: Api) -> list[HeaderPlan]:
    """One plan per header, in definition order."""
    includes = include_order(api)
    out = []
    for header in api.headers:
        path = header.path
        out.append(
            HeaderPlan(
                header=header,
                includes=includes[path],
                enums=tuple(e for e in api.enums if e.header == path),
                flags=tuple(f for f in api.flags if f.header == path),
                handles=tuple(h for h in api.handles if h.header == path),
                callbacks=tuple(c for c in api.callbacks if c.header == path),
                structs=_struct_order(api, path),
                functions=tuple(f for f in api.functions if f.header == path),
            )
        )
    return out
