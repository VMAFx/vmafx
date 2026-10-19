# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Compatibility of a CRD with the one it replaces (ADR-2350 D13).

Within a served version a custom resource only grows. `findings(old, new)`
lists what a client or a stored object written against `old` could no longer
use with `new`: a removed CRD, version, name, short name or property; a scope
change; a lost status subresource; a changed type; a new or changed format; a
new required property; an enum value removed, or an enum added where any value
was accepted; a raised minimum or minLength, a lowered maximum, maxLength or
maxItems, a new bound where there was none; a changed or new pattern; a
changed default. Descriptions, ordering and printer columns are free.
"""

from __future__ import annotations

from collections import deque
from typing import Any

Schema = dict[str, Any]
LOWER_BOUNDS = ("minimum", "minLength", "minItems")
UPPER_BOUNDS = ("maximum", "maxLength", "maxItems")
MAX_DEPTH = 32  # nesting of properties, items and maps (HISS-01: no unbounded walk)
MAX_NODES = 100_000  # schema nodes compared per version


def _bounds(old: Schema, new: Schema, path: str) -> list[str]:
    out = []
    for key in LOWER_BOUNDS:
        if key in new and (key not in old or new[key] > old[key]):
            out.append(f"{path}: {key} {old.get(key, 'none')} -> {new[key]} (narrower)")
    for key in UPPER_BOUNDS:
        if key in new and (key not in old or new[key] < old[key]):
            out.append(f"{path}: {key} {old.get(key, 'none')} -> {new[key]} (narrower)")
    return out


def _scalar_rules(old: Schema, new: Schema, path: str) -> list[str]:
    out = []
    if "type" in old and old["type"] != new.get("type"):
        out.append(f"{path}: type {old['type']} -> {new.get('type', 'none')}")
    if "format" in new and old.get("format") != new["format"]:
        out.append(f"{path}: format {old.get('format', 'none')} -> {new['format']}")
    if "enum" in new:
        lost = [v for v in old.get("enum", []) if v not in new["enum"]]
        if "enum" not in old or lost:
            out.append(f"{path}: enum narrowed (lost {lost or 'any value'})")
    if new.get("pattern") != old.get("pattern") and "pattern" in new:
        out.append(f"{path}: pattern {old.get('pattern', 'none')} -> {new['pattern']}")
    if "default" in old and old.get("default") != new.get("default"):
        out.append(f"{path}: default {old['default']!r} -> {new.get('default', 'none')!r}")
    if old.get("x-kubernetes-preserve-unknown-fields") and not new.get(
        "x-kubernetes-preserve-unknown-fields"
    ):
        out.append(f"{path}: unknown fields no longer preserved")
    return out + _bounds(old, new, path)


def _children(old: Schema, new: Schema) -> list[tuple[str, Schema, Schema | None]]:
    pairs: list[tuple[str, Schema, Schema | None]] = [
        (f".{name}", sub, new.get("properties", {}).get(name))
        for name, sub in old.get("properties", {}).items()
    ]
    if isinstance(old.get("items"), dict):
        pairs.append(("[]", old["items"], new.get("items")))
    if isinstance(old.get("additionalProperties"), dict):
        pairs.append(("{}", old["additionalProperties"], new.get("additionalProperties")))
    return pairs


def _node_findings(old: Schema, new: Schema, path: str) -> list[str]:
    out = _scalar_rules(old, new, path or ".")
    added = set(new.get("required", [])) - set(old.get("required", []))
    return out + [f"{path}.{name}: newly required" for name in sorted(added)]


def schema_findings(old: Schema, new: Schema, path: str = "") -> list[str]:
    """What new narrows or drops of old: both schemas walked side by side,
    breadth first, at most MAX_NODES nodes and MAX_DEPTH levels (HISS-01: an
    explicit queue, no recursion)."""
    out: list[str] = []
    pending: deque[tuple[str, Schema, Schema, int]] = deque([(path, old, new, 0)])
    for _ in range(MAX_NODES):
        if not pending:
            return out
        where, old_node, new_node, depth = pending.popleft()
        if depth > MAX_DEPTH:
            out.append(f"{where}: nested deeper than {MAX_DEPTH} levels")
            continue
        out += _node_findings(old_node, new_node, where)
        for suffix, old_sub, new_sub in _children(old_node, new_node):
            if isinstance(new_sub, dict):
                pending.append((where + suffix, old_sub, new_sub, depth + 1))
            else:
                out.append(f"{where}{suffix}: removed")
    if pending:
        out.append(f"{path or '.'}: more than {MAX_NODES} schema nodes")
    return out


def _versions(crd: Schema) -> dict[str, Schema]:
    return {v["name"]: v for v in crd.get("spec", {}).get("versions", [])}


def _names(old: Schema, new: Schema, name: str) -> list[str]:
    out = []
    old_names, new_names = old["spec"]["names"], new["spec"]["names"]
    for key in ("kind", "plural", "singular", "listKind"):
        if key in old_names and old_names[key] != new_names.get(key):
            out.append(f"{name}: names.{key} {old_names[key]} -> {new_names.get(key)}")
    lost = set(old_names.get("shortNames", [])) - set(new_names.get("shortNames", []))
    out += [f"{name}: short name {s} removed" for s in sorted(lost)]
    if old["spec"].get("scope") != new["spec"].get("scope"):
        out.append(f"{name}: scope {old['spec'].get('scope')} -> {new['spec'].get('scope')}")
    return out


def _version_findings(name: str, old: Schema, new: Schema) -> list[str]:
    out = []
    for version, old_v in _versions(old).items():
        new_v = _versions(new).get(version)
        where = f"{name}/{version}"
        if new_v is None:
            out.append(f"{where}: version removed")
            continue
        if old_v.get("served") and not new_v.get("served"):
            out.append(f"{where}: no longer served")
        if "status" in old_v.get("subresources", {}) and "status" not in new_v.get(
            "subresources", {}
        ):
            out.append(f"{where}: status subresource removed")
        old_s = old_v.get("schema", {}).get("openAPIV3Schema", {})
        new_s = new_v.get("schema", {}).get("openAPIV3Schema", {})
        out += [f"{where} {f}" for f in schema_findings(old_s, new_s)]
    return out


def findings(old: list[Schema], new: list[Schema]) -> list[str]:
    """Every incompatibility of the CRDs `new` with the CRDs `old`."""
    current = {c["metadata"]["name"]: c for c in new}
    out = []
    for crd in old:
        name = crd["metadata"]["name"]
        replacement = current.get(name)
        if replacement is None:
            out.append(f"{name}: CRD removed")
            continue
        out += _names(crd, replacement, name)
        out += _version_findings(name, crd, replacement)
    return out
