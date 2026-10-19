# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The Helm chart part of the platform definition (ADR-2350 D13).

Every key of deploy/helm/vmafx/values.yaml is one `[[chart]]` entry, written
in the order of the file: its dotted `path`, its default `value` (absent for a
mapping whose keys follow as their own entries, or for a key only the schema
knows), the comment and blank lines written above it (`lead`, verbatim with
their indentation), an inline `note` and the column of its `#`
(`note_column`), `quoted` for a string written in double quotes although it
needs none, `literal` for a multi-line string written as a `|` block, and the
key's JSON schema (`[chart.schema]`, every keyword but `properties`, which the
emitter builds from the entries below the key). `schema_order` lists a
mapping's schema properties when their order differs from the order of the
values file. `[chart_root]` holds the schema's root keywords, `schema_order`
for the top level and the comment lines after the last key (`values_tail`);
`[[chart_defs]]` are the schema's own `$defs`.

A `$ref` of `k8s:<name>` names a Kubernetes type of the minimum supported
minor (api/kubernetes/openapi-subset.json, written by
scripts/codegen/k8s_openapi.py); the emitter copies the type and every type it
refers to into `$defs`.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Any

from .entries import Entry, need
from .model import DefinitionError

K8S_PREFIX = "k8s:"
DEFS_PREFIX = "#/$defs/"
PATH_PART = re.compile(r'"([^"]+)"(?:\.|$)|([^."]+)(?:\.|$)')
MAX_SCHEMA_NODES = 100_000  # nodes walked per schema (HISS-01: an explicit stack)
ENTRY_KEYS = frozenset(
    {"path", "lead", "value", "quoted", "literal", "note", "note_column", "schema", "schema_order"}
)


class _NoValue:
    """The value of an entry that has none in the values file."""

    def __repr__(self) -> str:
        return "NO_VALUE"


NO_VALUE: Any = _NoValue()


@dataclass(frozen=True)
class ChartEntry:
    path: tuple[str, ...]
    lead: str
    value: Any
    quoted: bool
    literal: bool
    note: str
    note_column: int
    schema: dict[str, Any] | None
    schema_order: tuple[str, ...] | None

    @property
    def name(self) -> str:
        return join_path(self.path)

    @property
    def parent(self) -> tuple[str, ...]:
        return self.path[:-1]


@dataclass(frozen=True)
class Chart:
    root_schema: dict[str, Any]
    root_order: tuple[str, ...] | None
    values_tail: str
    defs: tuple[tuple[str, dict[str, Any]], ...]
    entries: tuple[ChartEntry, ...]


def _text(raw: Entry, key: str, where: str) -> str:
    value = raw.get(key, "")
    if not isinstance(value, str):
        raise DefinitionError(f"{where}: `{key}` is a string")
    return value


def _flag(raw: Entry, key: str, where: str) -> bool:
    value = raw.get(key, False)
    if not isinstance(value, bool):
        raise DefinitionError(f"{where}: `{key}` is true or false")
    return value


def _order(raw: Entry, where: str) -> tuple[str, ...] | None:
    order = raw.get("schema_order")
    if order is None:
        return None
    if not isinstance(order, list) or not all(isinstance(k, str) for k in order):
        raise DefinitionError(f"{where}: `schema_order` is a list of keys")
    return tuple(order)


def _schema(raw: Entry, where: str) -> dict[str, Any] | None:
    schema = raw.get("schema")
    if schema is None:
        return None
    if not isinstance(schema, dict):
        raise DefinitionError(f"{where}: `schema` is a table")
    if "properties" in schema:
        raise DefinitionError(f"{where}: `properties` come from the entries below the key")
    return dict(schema)


def split_path(path: str) -> tuple[str, ...] | None:
    """The keys of a dotted path; a key holding a dot is written in double
    quotes (`a."kubernetes.io/name"`). None for a malformed path."""
    keys: list[str] = []
    for part in PATH_PART.finditer(path):
        keys.append(part.group(1) if part.group(1) is not None else part.group(2))
    rebuilt = ".".join(f'"{k}"' if "." in k else k for k in keys)
    return tuple(keys) if keys and rebuilt == path and all(keys) else None


def join_path(keys: tuple[str, ...]) -> str:
    return ".".join(f'"{k}"' if "." in k else k for k in keys)


def _entry(raw: Entry) -> ChartEntry:
    path = need(raw, "path", "chart")
    where = f"chart[{path}]"
    keys = split_path(path) if isinstance(path, str) else None
    if keys is None:
        raise DefinitionError(f"{where}: `path` is a dotted key")
    unknown = set(raw) - ENTRY_KEYS
    if unknown:
        raise DefinitionError(f"{where}: unknown keys {sorted(unknown)}")
    column = raw.get("note_column", 0)
    if not isinstance(column, int) or isinstance(column, bool) or column < 0:
        raise DefinitionError(f"{where}: `note_column` is a column number")
    return ChartEntry(
        path=keys,
        lead=_text(raw, "lead", where),
        value=raw.get("value", NO_VALUE),
        quoted=_flag(raw, "quoted", where),
        literal=_flag(raw, "literal", where),
        note=_text(raw, "note", where),
        note_column=column,
        schema=_schema(raw, where),
        schema_order=_order(raw, where),
    )


def _defs(raw: list[Entry]) -> tuple[tuple[str, dict[str, Any]], ...]:
    out = []
    for entry in raw:
        name = need(entry, "name", "chart_defs")
        schema = need(entry, "schema", f"chart_defs[{name}]")
        if not isinstance(name, str) or not isinstance(schema, dict):
            raise DefinitionError(f"chart_defs[{name}]: a `name` and a `schema` table")
        out.append((name, dict(schema)))
    return tuple(out)


def parse_chart(data: dict[str, Any]) -> Chart | None:
    """The chart tables of the platform definition, validated; None without them."""
    if "chart" not in data and "chart_root" not in data:
        return None
    root = need(data, "chart_root", "platform definition")
    schema = need(root, "schema", "[chart_root]")
    if not isinstance(schema, dict) or "properties" in schema or "$defs" in schema:
        raise DefinitionError(
            "[chart_root.schema]: the root keywords, without properties and $defs"
        )
    chart = Chart(
        root_schema=dict(schema),
        root_order=_order(root, "[chart_root]"),
        values_tail=_text(root, "values_tail", "[chart_root]"),
        defs=_defs(data.get("chart_defs", [])),
        entries=tuple(_entry(e) for e in data.get("chart", [])),
    )
    validate_chart(chart)
    return chart


# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------


def schema_refs(schema: Any) -> list[str]:
    """Every `$ref` string in a schema, walked with an explicit stack."""
    refs: list[str] = []
    pending = [schema]
    for _ in range(MAX_SCHEMA_NODES):
        if not pending:
            return refs
        node = pending.pop()
        if isinstance(node, dict):
            ref = node.get("$ref")
            if isinstance(ref, str):
                refs.append(ref)
            pending.extend(node.values())
        elif isinstance(node, list):
            pending.extend(node)
    raise DefinitionError(f"a schema has more than {MAX_SCHEMA_NODES} nodes")


def _check_refs(chart: Chart) -> None:
    names = {name for name, _ in chart.defs}
    schemas = [("[chart_root]", chart.root_schema)] + [
        (f"chart_defs[{n}]", s) for n, s in chart.defs
    ]
    schemas += [(f"chart[{e.name}]", e.schema) for e in chart.entries if e.schema is not None]
    for where, schema in schemas:
        for ref in schema_refs(schema):
            if ref.startswith(K8S_PREFIX) and len(ref) > len(K8S_PREFIX):
                continue
            if not (ref.startswith(DEFS_PREFIX) and ref[len(DEFS_PREFIX) :] in names):
                raise DefinitionError(
                    f"{where}: $ref {ref} is neither a chart_defs name nor k8s:<type>"
                )


def _check_entry(entry: ChartEntry, has_value: bool) -> None:
    where = f"chart[{entry.name}]"
    is_string = isinstance(entry.value, str)
    if (entry.quoted or entry.literal) and not is_string:
        raise DefinitionError(f"{where}: `quoted` and `literal` are for a string value")
    if entry.literal and not entry.value.endswith("\n"):
        raise DefinitionError(f"{where}: a `literal` value ends with a newline")
    if (entry.note or entry.note_column) and not has_value:
        raise DefinitionError(f"{where}: a `note` belongs to a key with a value")
    if entry.note_column and not entry.note:
        raise DefinitionError(f"{where}: `note_column` without a `note`")
    for line in entry.lead.splitlines():
        if line.strip() and not line.lstrip().startswith("#"):
            raise DefinitionError(f"{where}: `lead` holds only comment and blank lines")


def _children(chart: Chart) -> dict[tuple[str, ...], list[ChartEntry]]:
    children: dict[tuple[str, ...], list[ChartEntry]] = {}
    seen: set[tuple[str, ...]] = {()}
    for entry in chart.entries:
        if entry.path in seen:
            raise DefinitionError(f"chart[{entry.name}]: the path appears twice")
        if entry.parent not in seen:
            raise DefinitionError(f"chart[{entry.name}]: its parent comes first")
        seen.add(entry.path)
        children.setdefault(entry.parent, []).append(entry)
    return children


def in_values(chart: Chart) -> set[tuple[str, ...]]:
    """The paths the values file holds: keys with a value and their parents."""
    paths: set[tuple[str, ...]] = set()
    for entry in chart.entries:
        if entry.value is not NO_VALUE:
            paths.update(entry.path[:n] for n in range(1, len(entry.path) + 1))
    return paths


def _check_tree(chart: Chart, children: dict[tuple[str, ...], list[ChartEntry]]) -> None:
    values = in_values(chart)
    by_path = {e.path: e for e in chart.entries}
    for entry in chart.entries:
        where = f"chart[{entry.name}]"
        below = children.get(entry.path, [])
        if entry.value is not NO_VALUE and any(c.path in values for c in below):
            raise DefinitionError(f"{where}: a key with a value has no keys with values below it")
        if entry.path not in values and entry.schema is None:
            raise DefinitionError(f"{where}: neither a value nor a schema")
        _check_entry(entry, entry.value is not NO_VALUE)
        with_schema = [c.path[-1] for c in below if c.schema is not None]
        if with_schema and entry.schema is not None and "$ref" in entry.schema:
            raise DefinitionError(f"{where}: a $ref schema has no properties below it")
        if entry.schema_order is not None and sorted(entry.schema_order) != sorted(with_schema):
            raise DefinitionError(f"{where}: `schema_order` names each key with a schema once")
    top = [e.path[-1] for e in children.get((), []) if e.schema is not None]
    if chart.root_order is not None and sorted(chart.root_order) != sorted(top):
        raise DefinitionError("[chart_root]: `schema_order` names each top-level key once")
    for entry in chart.entries:
        parent = by_path.get(entry.parent)
        if entry.schema is not None and parent is not None and parent.schema is None:
            raise DefinitionError(f"chart[{entry.name}]: its parent has no schema")


def validate_chart(chart: Chart) -> None:
    names = [n for n, _ in chart.defs]
    if len(set(names)) != len(names):
        raise DefinitionError("chart_defs: a name appears twice")
    _check_tree(chart, _children(chart))
    _check_refs(chart)
