# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""deploy/helm/vmafx/values.yaml and values.schema.json (ADR-2350 D13).

values.yaml is the `[[chart]]` entries in order: each entry's `lead` lines,
then the key and its value in block style (two spaces per level, list items
two spaces below their key, `|` blocks for `literal` strings), with its inline
`note`. A string is written plain unless YAML would read the plain text as
something else or the entry is `quoted`; then it is double-quoted.

values.schema.json is `[chart_root.schema]` with `properties` built from the
entries (in entry order, or a mapping's `schema_order`) and `$defs`: the
`[[chart_defs]]`, then every Kubernetes type a `k8s:` reference reaches, by
name. The layout is uniform: an object or array that fits on its line within
WIDTH columns is written on one line, any other is expanded, two spaces per
level. Every walk uses an explicit stack (HISS-01).
"""

from __future__ import annotations

import json
import re
from typing import Any

from .chart import (
    DEFS_PREFIX,
    K8S_PREFIX,
    MAX_SCHEMA_NODES,
    NO_VALUE,
    Chart,
    ChartEntry,
    in_values,
    schema_refs,
)
from .model import DefinitionError

VALUES = "deploy/helm/vmafx/values.yaml"
SCHEMA = "deploy/helm/vmafx/values.schema.json"
WIDTH = 100
INDENT = "  "
SPECIAL_WORDS = frozenset(
    {"true", "false", "yes", "no", "on", "off", "y", "n", "null", "~", ".inf", ".nan"}
)
NUMBER_LIKE = re.compile(
    r"[-+]?(\d[\d_]*(\.\d*)?|\.\d+)([eE][-+]?\d+)?|0[xob][0-9a-fA-F_]+|[-+]?\.(inf|nan)"
    r"|\d+(:\d+)+|\d{4}-\d\d-\d\d.*",
    re.IGNORECASE,
)
PLAIN_FIRST = re.compile(r"[A-Za-z0-9/._<>=+~$(]")
PLAIN_BODY = re.compile(r"[^\n\t\"'`]*")


# ---------------------------------------------------------------------------
# values.yaml
# ---------------------------------------------------------------------------


def plain_safe(text: str) -> bool:
    """Whether YAML reads `text`, written plain, back as the same string."""
    if not text or text != text.strip() or not PLAIN_FIRST.match(text):
        return False
    if not PLAIN_BODY.fullmatch(text) or ": " in text or " #" in text or text.endswith(":"):
        return False
    return text.lower() not in SPECIAL_WORDS and not NUMBER_LIKE.fullmatch(text)


def scalar(value: Any, quoted: bool = False) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int | float):
        return repr(value)
    if isinstance(value, str):
        if quoted or not plain_safe(value):
            return json.dumps(value, ensure_ascii=False)
        return value
    if value == {}:
        return "{}"
    if value == []:
        return "[]"
    raise DefinitionError(f"no plain YAML form for {value!r}")


def _is_block(value: Any) -> bool:
    return isinstance(value, dict | list) and bool(value)


def block_lines(key: str, value: Any, indent: int) -> list[str]:
    """`key: value` at `indent` spaces, nested mappings and lists in block style."""
    out: list[str] = []
    # (indent, prefix, key, value): prefix replaces the indent of the first line ("- ").
    pending: list[tuple[int, str, str, Any]] = [(indent, " " * indent, key, value)]
    for _ in range(MAX_SCHEMA_NODES):
        if not pending:
            return out
        level, prefix, name, item = pending.pop()
        head = f"{prefix}{name}:" if name else prefix.rstrip()
        if not _is_block(item):
            out.append(f"{prefix}{name}: {scalar(item)}" if name else f"{prefix}{scalar(item)}")
            continue
        if name:
            out.append(head)
        tasks = _block_items(level, name, item)
        pending.extend(reversed(tasks))
    raise DefinitionError(f"{key}: deeper than {MAX_SCHEMA_NODES} nodes")


def _block_items(level: int, name: str, item: Any) -> list[tuple[int, str, str, Any]]:
    child = level + 2 if name else level
    if isinstance(item, dict):
        return [(child, " " * child, k, v) for k, v in item.items()]
    tasks: list[tuple[int, str, str, Any]] = []
    for element in item:
        dash = " " * child + "- "
        if isinstance(element, dict) and element:
            keys = list(element.items())
            tasks.append((child + 2, dash, keys[0][0], keys[0][1]))
            tasks += [(child + 2, " " * (child + 2), k, v) for k, v in keys[1:]]
        elif isinstance(element, list):
            raise DefinitionError("a list directly inside a list has no block form here")
        else:
            tasks.append((child, dash, "", element))
    return tasks


def _entry_lines(entry: ChartEntry) -> list[str]:
    indent = INDENT * (len(entry.path) - 1)
    key = entry.path[-1]
    if entry.value is NO_VALUE:
        return [f"{indent}{key}:"]
    if entry.literal:
        body = [f"{indent}{INDENT}{line}" if line else "" for line in entry.value[:-1].split("\n")]
        return [f"{indent}{key}: |", *body]
    if _is_block(entry.value):
        return block_lines(key, entry.value, len(indent))
    line = f"{indent}{key}: {scalar(entry.value, entry.quoted)}"
    if entry.note:
        column = entry.note_column or len(line) + 2
        line = (
            f"{line.ljust(column - 1)} # {entry.note}"
            if len(line) < column
            else f"{line} # {entry.note}"
        )
    return [line]


def values_text(chart: Chart) -> str:
    """deploy/helm/vmafx/values.yaml."""
    shown = in_values(chart)
    out: list[str] = []
    for entry in chart.entries:
        if entry.path not in shown:
            continue
        out.append(entry.lead)
        out.append("".join(f"{line}\n" for line in _entry_lines(entry)))
    out.append(chart.values_tail)
    return "".join(out)


# ---------------------------------------------------------------------------
# values.schema.json
# ---------------------------------------------------------------------------


def _resolve(node: Any) -> Any:
    """A copy of a schema with `k8s:` references written as `#/$defs/`."""
    text = json.dumps(node, ensure_ascii=False)
    copy = json.loads(text)
    pending = [copy]
    for _ in range(MAX_SCHEMA_NODES):
        if not pending:
            return copy
        item = pending.pop()
        if isinstance(item, dict):
            ref = item.get("$ref")
            if isinstance(ref, str) and ref.startswith(K8S_PREFIX):
                item["$ref"] = DEFS_PREFIX + ref[len(K8S_PREFIX) :]
            pending.extend(item.values())
        elif isinstance(item, list):
            pending.extend(item)
    raise DefinitionError(f"a schema has more than {MAX_SCHEMA_NODES} nodes")


def kubernetes_defs(chart: Chart, kubernetes: dict[str, Any]) -> dict[str, Any]:
    """Every Kubernetes type the chart's `k8s:` references reach, by name."""
    sources = [chart.root_schema, *(s for _, s in chart.defs)]
    sources += [e.schema for e in chart.entries if e.schema is not None]
    pending = [
        r[len(K8S_PREFIX) :] for s in sources for r in schema_refs(s) if r.startswith(K8S_PREFIX)
    ]
    found: dict[str, Any] = {}
    for _ in range(MAX_SCHEMA_NODES):
        if not pending:
            return {name: found[name] for name in sorted(found)}
        name = pending.pop()
        if name in found:
            continue
        if name not in kubernetes:
            raise DefinitionError(
                f"Kubernetes type {name} is not in api/kubernetes/openapi-subset.json; "
                "run python3 scripts/codegen/k8s_openapi.py --write"
            )
        found[name] = kubernetes[name]
        pending += [r[len(DEFS_PREFIX) :] for r in schema_refs(kubernetes[name])]
    raise DefinitionError(f"more than {MAX_SCHEMA_NODES} Kubernetes types")


def _ordered(
    chart: Chart, nodes: dict[tuple[str, ...], dict[str, Any]], root: dict[str, Any]
) -> None:
    orders = [((), chart.root_order)] + [(e.path, e.schema_order) for e in chart.entries]
    for path, order in orders:
        node = root if path == () else nodes.get(path)
        if order is None or node is None or "properties" not in node:
            continue
        node["properties"] = {key: node["properties"][key] for key in order}


def schema_value(chart: Chart, kubernetes: dict[str, Any]) -> dict[str, Any]:
    """The JSON value of values.schema.json."""
    root: dict[str, Any] = _resolve(chart.root_schema)
    nodes: dict[tuple[str, ...], dict[str, Any]] = {}
    for entry in chart.entries:
        if entry.schema is None:
            continue
        node: dict[str, Any] = _resolve(entry.schema)
        nodes[entry.path] = node
        parent = root if entry.parent == () else nodes[entry.parent]
        parent.setdefault("properties", {})[entry.path[-1]] = node
    _ordered(chart, nodes, root)
    defs = {name: _resolve(schema) for name, schema in chart.defs}
    defs.update(kubernetes_defs(chart, kubernetes))
    root["$defs"] = defs
    return root


def _compact(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(", ", ": "))


def layout(value: Any) -> str:
    """JSON text: one line per object or array that fits within WIDTH, else expanded."""
    out: list[str] = []
    # (indent, prefix, value, suffix) or a finished line as (indent, line, None, None)
    pending: list[tuple[int, str, Any, str | None]] = [(0, "", value, "")]
    for _ in range(MAX_SCHEMA_NODES):
        if not pending:
            return "\n".join(out) + "\n"
        level, prefix, item, suffix = pending.pop()
        pad = INDENT * level
        if suffix is None:
            out.append(pad + prefix)
            continue
        one = f"{pad}{prefix}{_compact(item)}{suffix}"
        if not isinstance(item, dict | list) or not item or len(one) <= WIDTH:
            out.append(one)
            continue
        opening, closing = ("{", "}") if isinstance(item, dict) else ("[", "]")
        out.append(f"{pad}{prefix}{opening}")
        pending.append((level, closing + suffix, None, None))
        members = list(item.items()) if isinstance(item, dict) else [("", v) for v in item]
        last = len(members) - 1
        for i, (key, member) in reversed(list(enumerate(members))):
            head = f"{json.dumps(key, ensure_ascii=False)}: " if isinstance(item, dict) else ""
            pending.append((level + 1, head, member, "" if i == last else ","))
    raise DefinitionError(f"a schema has more than {MAX_SCHEMA_NODES} nodes")


def schema_text(chart: Chart, kubernetes: dict[str, Any]) -> str:
    """deploy/helm/vmafx/values.schema.json."""
    return layout(schema_value(chart, kubernetes))


def files(chart: Chart, kubernetes: dict[str, Any]) -> dict[str, str]:
    return {VALUES: values_text(chart), SCHEMA: schema_text(chart, kubernetes)}
