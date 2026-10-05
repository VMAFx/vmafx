# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Append-only check of the definition against an earlier version (HISS-14).

Breaking: a removed function, compat shim, handle, struct, field, constant or
status; a changed parameter list or result; a reordered, retyped or renamed
field; a field added to a struct without `struct_size`; a renumbered constant.
A breaking change passes only with a higher ABI major. An addition needs a
higher ABI version than the earlier definition.
"""

from __future__ import annotations

from .model import Api, Function, Struct


def _signature(fn: Function) -> tuple[object, ...]:
    return (fn.returns, tuple((p.type, p.mode) for p in fn.params))


def _functions(old: Api, new: Api) -> tuple[list[str], list[str]]:
    breaking, added = [], []
    current = {fn.name: fn for fn in new.functions}
    for fn in old.functions:
        if fn.name not in current:
            breaking.append(f"function {fn.name} removed")
        elif _signature(current[fn.name]) != _signature(fn):
            breaking.append(f"function {fn.name}: parameters or result changed")
    previous = {fn.name for fn in old.functions}
    added += [f"function {fn.name}" for fn in new.functions if fn.name not in previous]
    previous_compat = {c.name for c in old.compats}
    current_compat = {c.name for c in new.compats}
    breaking += [f"compat shim {n} removed" for n in sorted(previous_compat - current_compat)]
    return breaking, added


def _fields(old: Struct, new: Struct) -> tuple[list[str], list[str]]:
    breaking, added = [], []
    if old.sized != new.sized:
        breaking.append(f"struct {old.name}: `sized` changed")
    previous = [(f.name, f.type, f.enum) for f in old.fields]
    current = [(f.name, f.type, f.enum) for f in new.fields]
    if current[: len(previous)] != previous:
        breaking.append(f"struct {old.name}: field removed, reordered, renamed or retyped")
    elif len(current) > len(previous):
        if not new.sized:
            breaking.append(f"struct {old.name}: grew without struct_size")
        added.append(f"struct {old.name}: {len(current) - len(previous)} field(s) appended")
    return breaking, added


def _structs(old: Api, new: Api) -> tuple[list[str], list[str]]:
    breaking, added = [], []
    current = {s.name: s for s in new.structs}
    for item in old.structs:
        if item.name not in current:
            breaking.append(f"struct {item.name} removed")
            continue
        more_breaking, more_added = _fields(item, current[item.name])
        breaking += more_breaking
        added += more_added
    previous = {s.name for s in old.structs}
    added += [f"struct {s.name}" for s in new.structs if s.name not in previous]
    handles = {h.name for h in new.handles}
    breaking += [f"handle {h.name} removed" for h in old.handles if h.name not in handles]
    return breaking, added


def _constants(old: Api, new: Api) -> tuple[list[str], list[str]]:
    def table(api: Api) -> dict[str, int]:
        values = {s.name: s.value for s in api.statuses}
        for enum in api.enums:
            values.update({v.name: v.value for v in enum.values})
        return values

    previous, current = table(old), table(new)
    breaking = []
    for name, value in previous.items():
        if name not in current:
            breaking.append(f"constant {name} removed")
        elif current[name] != value:
            breaking.append(f"constant {name} renumbered {value} -> {current[name]}")
    added = [f"constant {name}" for name in current if name not in previous]
    return breaking, added


def compare(old: Api, new: Api) -> list[str]:
    """Findings that make `new` unacceptable as a successor of `old`; empty = fine."""
    breaking, added = [], []
    for check in (_functions, _structs, _constants):
        more_breaking, more_added = check(old, new)
        breaking += more_breaking
        added += more_added
    findings = []
    if breaking and new.abi_version[0] <= old.abi_version[0]:
        findings += [f"breaking: {item} (needs a higher ABI major)" for item in breaking]
    if added and not breaking and new.abi_version <= old.abi_version:
        findings += [f"addition without an ABI version bump: {item}" for item in added]
    return findings
