# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Append-only check of the definition against an earlier version (HISS-14).

Breaking: a removed function, compat shim, handle, callback, struct, field,
constant, flag bit or option; a changed parameter list, result or callback
signature; a reordered, retyped or renamed field; a field added to a struct
without `struct_size` or to a struct another struct embeds by value; a
renumbered constant or bit; a changed `since`; a changed option type or proto
field number. A break passes only with a higher ABI major, or within 0.x with a
higher minor (ADR-1852: the ABI freezes at `v1.0.0`); the CLI lists every
accepted break so the PR body can name it.

An addition needs a higher ABI version than the earlier definition. Its
`since` names the current minor or a newer one; from 1.0 it must be newer,
because a version node that shipped never gains a symbol.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from .model import Api, Callback, Function, Struct, version_text
from .validate import constants

Change = tuple[str, tuple[int, int] | None]  # (description, since of an addition)


@dataclass
class Report:
    findings: list[str] = field(default_factory=list)  # refuse the new definition
    accepted: list[str] = field(default_factory=list)  # breaks a version bump allows
    added: list[str] = field(default_factory=list)


def _signature(fn: Function | Callback) -> tuple[object, ...]:
    return (fn.returns, tuple((p.type, p.mode) for p in fn.params))


def _named(
    old: list[Function] | list[Callback], new: list[Function] | list[Callback], what: str
) -> tuple[list[str], list[Change]]:
    breaking: list[str] = []
    current = {item.name: item for item in new}
    for item in old:
        now = current.get(item.name)
        if now is None:
            breaking.append(f"{what} {item.name} removed")
        elif _signature(now) != _signature(item):
            breaking.append(f"{what} {item.name}: parameters or result changed")
        elif now.since != item.since:
            breaking.append(
                f"{what} {item.name}: since changed {version_text(item.since)} -> {version_text(now.since)}"
            )
    previous = {item.name for item in old}
    added: list[Change] = [
        (f"{what} {item.name}", item.since) for item in new if item.name not in previous
    ]
    return breaking, added


def _functions(old: Api, new: Api) -> tuple[list[str], list[Change]]:
    breaking, added = _named(list(old.functions), list(new.functions), "function")
    more_breaking, more_added = _named(list(old.callbacks), list(new.callbacks), "callback")
    current_compat = {c.name for c in new.compats}
    breaking += more_breaking + [
        f"compat shim {c.name} removed" for c in old.compats if c.name not in current_compat
    ]
    handles = {h.name for h in new.handles}
    breaking += [f"handle {h.name} removed" for h in old.handles if h.name not in handles]
    previous = {h.name for h in old.handles}
    added += more_added + [
        (f"handle {h.name}", h.since) for h in new.handles if h.name not in previous
    ]
    return breaking, added


def _embedded(api: Api) -> dict[str, str]:
    """Struct name -> a struct that embeds it by value."""
    return {f.type: s.name for s in api.structs for f in s.fields if api.is_struct(f.type)}


def _fields(old: Struct, new: Struct, embedded_in: str | None) -> tuple[list[str], list[Change]]:
    breaking: list[str] = []
    if old.sized != new.sized:
        breaking.append(f"struct {old.name}: `sized` changed")
    previous = [f.abi_key() for f in old.fields]
    if [f.abi_key() for f in new.fields[: len(previous)]] != previous:
        return [*breaking, f"struct {old.name}: field removed, reordered, renamed or retyped"], []
    appended = new.fields[len(previous) :]
    if appended and not new.sized:
        breaking.append(f"struct {old.name}: grew without struct_size")
    if appended and embedded_in:
        breaking.append(f"struct {old.name}: grew while {embedded_in} embeds it by value")
    added: list[Change] = [(f"field {old.name}.{f.name}", f.since) for f in appended]
    return breaking, added


def _structs(old: Api, new: Api) -> tuple[list[str], list[Change]]:
    breaking: list[str] = []
    added: list[Change] = []
    current = {s.name: s for s in new.structs}
    embedded = _embedded(old)
    for item in old.structs:
        if item.name not in current:
            breaking.append(f"struct {item.name} removed")
            continue
        more_breaking, more_added = _fields(item, current[item.name], embedded.get(item.name))
        breaking += more_breaking
        added += more_added
    previous = {s.name for s in old.structs}
    added += [(f"struct {s.name}", s.since) for s in new.structs if s.name not in previous]
    return breaking, added


def _member_since(api: Api) -> dict[str, tuple[int, int]]:
    out = {s.name: s.since for s in api.statuses}
    out.update({v.name: v.since for e in api.enums for v in e.values})
    out.update({b.name: b.since for f in api.flags for b in f.bits})
    return out


def _constants(old: Api, new: Api) -> tuple[list[str], list[Change]]:
    previous, current = constants(old), constants(new)
    breaking = []
    for name, value in previous.items():
        if name not in current:
            breaking.append(f"constant {name} removed")
        elif current[name] != value:
            breaking.append(f"constant {name} renumbered {value} -> {current[name]}")
    since = _member_since(new)
    added: list[Change] = [
        (f"constant {name}", since[name]) for name in current if name not in previous
    ]
    return breaking, added


def _options(old: Api, new: Api) -> tuple[list[str], list[Change]]:
    def table(api: Api) -> dict[str, tuple[object, ...]]:
        return {
            f"{g.name}.{o.name}": (o.type, o.proto_field, o.spellings)
            for g in api.option_groups
            for o in g.options
        }

    previous, current = table(old), table(new)
    breaking = []
    for name, (kind, proto, spellings) in previous.items():
        now = current.get(name)
        if now is None:
            breaking.append(f"option {name} removed")
        elif now[:2] != (kind, proto):
            breaking.append(f"option {name}: type or proto field changed")
        elif any(not set(v) <= set(now[2].get(k, ())) for k, v in spellings.items()):  # type: ignore[attr-defined]
            breaking.append(f"option {name}: a surface spelling was removed")
    return breaking, [(f"option {name}", None) for name in current if name not in previous]


def _break_allowed(old: Api, new: Api) -> bool:
    if old.abi_version[0] == 0:
        return new.abi_minor_node > old.abi_minor_node
    return new.abi_version[0] > old.abi_version[0]


def _addition_findings(old: Api, new: Api, added: list[Change]) -> list[str]:
    """An addition bumps the ABI; its node is the current minor or newer.

    From 1.0 a node that shipped is frozen: the addition's `since` must be a
    newer minor than the earlier ABI. Within the 0.x preview (ADR-1852 D2) the
    current minor's node may still grow, so parallel work packages add with a
    patch bump instead of each claiming a minor.
    """
    out = []
    if added and new.abi_version <= old.abi_version:
        out += [f"addition without an ABI version bump: {text}" for text, _ in added]
    current = old.abi_minor_node
    frozen = old.abi_version[0] >= 1
    for text, since in added:
        if since is None or since > current or (since == current and not frozen):
            continue
        state = "shipped and is frozen" if frozen else "is older than the current minor"
        out.append(
            f"addition {text}: since {version_text(since)}: node {version_text(since)} {state}"
        )
    return out


def report(old: Api, new: Api) -> Report:
    """Compare `new` with `old`: refused findings, accepted breaks, additions."""
    breaking: list[str] = []
    added: list[Change] = []
    for check in (_functions, _structs, _constants, _options):
        more_breaking, more_added = check(old, new)
        breaking += more_breaking
        added += more_added
    result = Report(added=[text for text, _ in added])
    if breaking and _break_allowed(old, new):
        result.accepted = breaking
    elif breaking:
        rule = "a higher ABI minor (0.x)" if old.abi_version[0] == 0 else "a higher ABI major"
        result.findings += [f"breaking: {item} (needs {rule})" for item in breaking]
    if not breaking:
        result.findings += _addition_findings(old, new, added)
    return result


def compare(old: Api, new: Api) -> list[str]:
    """Findings that make `new` unacceptable as a successor of `old`; empty = fine."""
    return report(old, new).findings
