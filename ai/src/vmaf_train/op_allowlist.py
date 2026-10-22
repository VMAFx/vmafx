# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Parse libvmaf's ONNX op allowlist and check models against it.

Single source of truth is core/src/dnn/op_allowlist.c — parsing the C
string table keeps Python and C from drifting. Any model whose graph uses
an op not on the list will be rejected by vmaf_dnn_op_allowed() at
session-init time, so catching it at export time turns a runtime load
failure into a trainer-side error.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import onnx

_ALLOWLIST_C = Path(__file__).resolve().parents[3] / "core/src/dnn/op_allowlist.c"
_STRING_RE = re.compile(r'"([A-Za-z][A-Za-z0-9_]*)"')


def load_allowlist(source: Path = _ALLOWLIST_C) -> frozenset[str]:
    """Return the set of op types libvmaf will accept at session init."""
    if not source.is_file():
        raise FileNotFoundError(f"op_allowlist.c not found at {source}; cannot validate ONNX ops")
    text = source.read_text(encoding="utf-8")
    return frozenset(_STRING_RE.findall(text))


#: Hard ceiling on a static `Loop.M` trip count (ADR-0171 / T6-5b).
#: Production models — diffusion, RAFT, MUSIQ — fit well under this; pick
#: deliberately so an attacker can't point a Loop at `int64.MAX` without
#: tripping the bound.
MAX_LOOP_TRIP_COUNT = 1024


@dataclass(frozen=True)
class AllowlistReport:
    allowed: frozenset[str]
    used: frozenset[str]
    forbidden: frozenset[str]
    #: Per-Loop diagnostic; empty when no Loop nodes exist or every Loop
    #: traces to a Constant trip count <= MAX_LOOP_TRIP_COUNT.
    loop_violations: tuple[str, ...] = ()

    @property
    def ok(self) -> bool:
        return not self.forbidden and not self.loop_violations

    def pretty(self) -> str:
        if self.ok:
            return f"allowlist OK ({len(self.used)} distinct ops, all allowed)"
        parts: list[str] = []
        if self.forbidden:
            bad = ", ".join(sorted(self.forbidden))
            parts.append(f"{len(self.forbidden)} forbidden op(s): {bad}")
        if self.loop_violations:
            parts.append(
                f"{len(self.loop_violations)} unbounded Loop(s): " + "; ".join(self.loop_violations)
            )
        return "allowlist FAIL: " + " | ".join(parts)


# Upper bound on the graphs and nodes one model walk visits.
_WALK_LIMIT = 10_000_000


def _subgraphs(node: "onnx.NodeProto", scope: str) -> list[tuple["onnx.GraphProto", str]]:
    """Return the embedded subgraphs of ``node`` with the scope label of each."""
    found: list[tuple["onnx.GraphProto", str]] = []
    for attr in node.attribute:
        if attr.type == onnx.AttributeProto.GRAPH:
            found.append((attr.g, f"{scope}::{node.op_type}.{attr.name}"))
        elif attr.type == onnx.AttributeProto.GRAPHS:
            for i, sub in enumerate(attr.graphs):
                found.append((sub, f"{scope}::{node.op_type}.{attr.name}[{i}]"))
    return found


def _collect_op_types(graph: "onnx.GraphProto") -> set[str]:
    """Walk a GraphProto and collect every op_type, descending into the
    embedded subgraphs of control-flow ops (Loop.body, If.then_branch,
    If.else_branch) with an explicit work stack (HISS-01). Mirrors the
    C-side scanner in `core/src/dnn/onnx_scan.c` so the export-time check
    and the runtime load-time check stay in lockstep (ADR-0169 / T6-5)."""
    used: set[str] = set()
    pending = [graph]
    for _ in range(_WALK_LIMIT):
        if not pending:
            return used
        current = pending.pop()
        for node in current.node:
            used.add(node.op_type)
            pending.extend(sub for sub, _ in _subgraphs(node, ""))
    raise ValueError("ONNX graph is too large to scan")


def _constant_int64_value(node: "onnx.NodeProto") -> int | None:
    """Extract the int64 scalar value from a `Constant` node, or return
    None if the node isn't a scalar int64 Constant. ADR-0171 / T6-5b
    relies on this to bound `Loop.M` trip counts.

    A `Constant` node carries its tensor through one of the attributes
    `value` (TensorProto) or, in newer opsets, the value-typed
    `value_int` / `value_ints` / `value_floats` etc. We only accept
    `value` of dtype INT64 with a single element here — anything more
    elaborate counts as "not statically bounded" for our purposes.
    """
    if node.op_type != "Constant":
        return None
    for attr in node.attribute:
        if attr.name != "value" or attr.type != onnx.AttributeProto.TENSOR:
            continue
        tensor = attr.t
        if tensor.data_type != onnx.TensorProto.INT64:
            continue
        # Two encodings — int64_data (repeated) or raw_data (little-endian bytes).
        if tensor.int64_data:
            return int(tensor.int64_data[0])
        if tensor.raw_data:
            import struct

            if len(tensor.raw_data) >= 8:
                return int(struct.unpack("<q", tensor.raw_data[:8])[0])
        return None
    return None


def _loop_violation(
    node: "onnx.NodeProto",
    producers: dict[str, "onnx.NodeProto"],
    *,
    max_trip_count: int,
    scope: str,
) -> str | None:
    """Return the diagnostic for a `Loop` node that is not statically bounded."""
    if not node.input:
        return f"{scope}::Loop(no inputs)"
    m_input = node.input[0]
    producer = producers.get(m_input)
    if producer is None:
        return f"{scope}::Loop(M={m_input!r} is a graph input, not a Constant)"
    val = _constant_int64_value(producer)
    if val is None:
        return f"{scope}::Loop(M traces to {producer.op_type!r}, not a scalar int64 Constant)"
    if val < 0 or val > max_trip_count:
        return f"{scope}::Loop(M={val}, max_trip_count={max_trip_count})"
    return None


# One unit of the violation walk: a graph to open, or a Loop node to check.
_WalkItem = tuple[str, Any, Any]


def _expand_graph(
    graph: "onnx.GraphProto", scope: str, max_trip_count: int, pending: list[_WalkItem]
) -> None:
    """Queue the Loop checks and subgraphs of ``graph`` so they pop in source order."""
    # Build an output-name -> producer-node map for this scope. ONNX
    # requires nodes in topological order, but a forward map costs us
    # nothing and makes the lookup obvious.
    producers: dict[str, "onnx.NodeProto"] = {}
    for node in graph.node:
        for out in node.output:
            producers[out] = node

    items: list[_WalkItem] = []
    for node in graph.node:
        if node.op_type == "Loop":
            items.append(("loop", node, (producers, scope, max_trip_count)))
        # Subgraphs are walked regardless of op_type.
        items.extend(("graph", sub, sub_scope) for sub, sub_scope in _subgraphs(node, scope))
    pending.extend(reversed(items))


def _collect_loop_violations(
    graph: "onnx.GraphProto", *, max_trip_count: int = MAX_LOOP_TRIP_COUNT, scope: str = "<top>"
) -> list[str]:
    """Walk a GraphProto and surface every `Loop` whose first input does
    not trace to a `Constant` int64 scalar with value in [0, max_trip_count].

    Inner subgraphs are walked too (explicit work stack, HISS-01); a Loop
    body is checked in isolation against the same rule (a Loop nested inside
    a Loop must itself be statically bounded).

    Returns a list of human-readable diagnostics, in source order.
    """
    violations: list[str] = []
    pending: list[_WalkItem] = [("graph", graph, scope)]
    for _ in range(_WALK_LIMIT):
        if not pending:
            return violations
        kind, payload, extra = pending.pop()
        if kind == "graph":
            _expand_graph(payload, extra, max_trip_count, pending)
            continue
        producers, loop_scope, limit = extra
        found = _loop_violation(payload, producers, max_trip_count=limit, scope=loop_scope)
        if found is not None:
            violations.append(found)
    raise ValueError("ONNX graph is too large to scan")


def check_model(
    onnx_path: Path,
    allowed: frozenset[str] | None = None,
    max_loop_trip_count: int = MAX_LOOP_TRIP_COUNT,
) -> AllowlistReport:
    """Walk @p onnx_path and compare its ops against libvmaf's allowlist."""
    model = onnx.load(str(onnx_path))
    return check_graph(model, allowed, max_loop_trip_count)


def check_graph(
    model: onnx.ModelProto,
    allowed: frozenset[str] | None = None,
    max_loop_trip_count: int = MAX_LOOP_TRIP_COUNT,
) -> AllowlistReport:
    """Same as check_model() but against an in-memory onnx.ModelProto."""
    if allowed is None:
        allowed = load_allowlist()
    used = frozenset(_collect_op_types(model.graph))
    loop_violations = tuple(
        _collect_loop_violations(model.graph, max_trip_count=max_loop_trip_count)
    )
    return AllowlistReport(
        allowed=allowed,
        used=used,
        forbidden=frozenset(op for op in used if op not in allowed),
        loop_violations=loop_violations,
    )
