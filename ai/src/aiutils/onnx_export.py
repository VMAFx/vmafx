# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Warning-free ONNX export on the PyTorch dynamo exporter.

PyTorch 2.14's dynamo exporter implements its operators at opset 18. Asked for
the fork's opset-17 model contract it logs a warning, attempts its own
conversion, and silently leaves the graph at 18 when that conversion fails, so
a published model can claim an opset it is not. This module exports at the
native opset, converts explicitly, validates the result, and atomically
publishes one self-contained file instead.

The exporter's own axis-renaming pass is deliberately not used. It is driven by
the exported symbols rather than by the caller's inputs, so two tensors that
share one dimension (a common batch axis, a shared frame geometry) resolve to a
single symbol and the second name is rejected with a ``UserWarning``. This
module therefore asks ``torch.export`` only to keep the axes dynamic and then
applies the caller's names to the serialized graph, where one symbol shared by
several tensors names every one of them exactly once.

ONNX's own version converter is likewise not trusted blindly. Down-converting a
reduction operator moves ``axes`` from an input back to an attribute but leaves
``noop_with_empty_axes`` in place, which the target opset does not define, so
the result declares the requested opset while failing ONNX's checker. Leftovers
that still carry their source-opset default are dropped, anything else is
reported, and the finished graph is validated before it is published.
"""

from __future__ import annotations

import os
import tempfile
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any

import onnx
import torch

_DYNAMO_EXPORT_OPSET = 18


def _dynamic_shapes(
    example_inputs: tuple[Any, ...],
    input_names: Sequence[str],
    dynamic_axes: Mapping[str, Mapping[int, str]] | None,
) -> tuple[dict[int, Any] | None, ...] | None:
    """Translate the legacy named-axis contract into positional dynamism hints."""
    if dynamic_axes is None:
        return None
    if len(example_inputs) != len(input_names):
        raise ValueError("ONNX input_names must match the positional example inputs")

    shapes: list[dict[int, Any] | None] = []
    has_dynamic_input = False
    for value, name in zip(example_inputs, input_names, strict=True):
        axes = dynamic_axes.get(name, {})
        if not axes:
            shapes.append(None)
            continue
        ndim = int(getattr(value, "ndim", -1))
        spec: dict[int, Any] = {}
        for axis in axes:
            if axis < 0 or axis >= ndim:
                raise ValueError(f"dynamic axis {axis} is outside {name!r} rank {ndim}")
            spec[axis] = torch.export.Dim.DYNAMIC
        shapes.append(spec)
        has_dynamic_input = True
    return tuple(shapes) if has_dynamic_input else None


def _axis_labels(
    graph: onnx.GraphProto,
    dynamic_axes: Mapping[str, Mapping[int, str]],
) -> dict[str, str]:
    """Resolve each exported symbolic dimension to the caller's axis label."""
    labels: dict[str, str] = {}
    inputs = {value.name for value in graph.input}
    for value in list(graph.input) + list(graph.output):
        axes = dynamic_axes.get(value.name)
        if not axes:
            continue
        dimensions = value.type.tensor_type.shape.dim
        for axis, label in axes.items():
            if axis < 0 or axis >= len(dimensions):
                raise ValueError(f"dynamic axis {axis} is outside {value.name!r} rank")
            dimension = dimensions[axis]
            if dimension.WhichOneof("value") != "dim_param":
                if value.name in inputs:
                    raise ValueError(f"export pinned {value.name!r} axis {axis} to a fixed extent")
                continue
            previous = labels.setdefault(dimension.dim_param, label)
            if previous != label:
                raise ValueError(f"axes {previous!r} and {label!r} share one exported dimension")
    return labels


def _schema_attributes(op_type: str, domain: str, opset: int) -> Mapping[str, Any] | None:
    """Attribute specs for *op_type* at *opset*, or None when it has no schema there."""
    try:
        return dict(onnx.defs.get_schema(op_type, opset, domain).attributes)
    except onnx.defs.SchemaError:
        return None


def _is_schema_default(spec: Any, attribute: onnx.AttributeProto) -> bool:
    """True when *attribute* still carries the value its schema defaults to."""
    if spec is None:
        return False
    default = spec.default_value
    if default.type != attribute.type:
        return False
    candidate = onnx.AttributeProto()
    candidate.CopyFrom(attribute)
    candidate.name = default.name
    return bool(candidate == default)


def _prune_converted_attributes(graph: onnx.GraphProto, source: int, target: int) -> None:
    """Drop the no-op attributes ONNX's version converter leaves behind."""
    for node in graph.node:
        target_attributes = _schema_attributes(node.op_type, node.domain, target)
        if target_attributes is None:
            continue
        source_attributes = _schema_attributes(node.op_type, node.domain, source)
        stale = [item for item in node.attribute if item.name not in target_attributes]
        for attribute in stale:
            spec = None if source_attributes is None else source_attributes.get(attribute.name)
            if not _is_schema_default(spec, attribute):
                raise ValueError(
                    f"{node.op_type} attribute {attribute.name!r} has no opset-{target} "
                    "form and does not carry its default value"
                )
            node.attribute.remove(attribute)
        for attribute in node.attribute:
            subgraphs = list(attribute.graphs)
            if attribute.HasField("g"):
                subgraphs.append(attribute.g)
            for subgraph in subgraphs:
                _prune_converted_attributes(subgraph, source, target)


def _apply_axis_labels(graph: onnx.GraphProto, labels: Mapping[str, str]) -> None:
    """Rename every occurrence of a labelled symbolic dimension in *graph*."""
    if not labels:
        return
    for value in list(graph.input) + list(graph.output) + list(graph.value_info):
        for dimension in value.type.tensor_type.shape.dim:
            if dimension.WhichOneof("value") != "dim_param":
                continue
            label = labels.get(dimension.dim_param)
            if label is not None:
                dimension.dim_param = label


def export_onnx(
    model: Any,
    example_inputs: tuple[Any, ...],
    output: str | os.PathLike[str],
    *,
    input_names: Sequence[str],
    output_names: Sequence[str],
    dynamic_axes: Mapping[str, Mapping[int, str]] | None = None,
    opset: int = 17,
) -> None:
    """Export through dynamo and atomically publish the requested ONNX opset."""
    output_path = Path(output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    export_opset = max(opset, _DYNAMO_EXPORT_OPSET)
    dynamic_shapes = _dynamic_shapes(example_inputs, input_names, dynamic_axes)

    handle, staging_name = tempfile.mkstemp(
        prefix=f".{output_path.name}.dynamo-", suffix=".onnx", dir=output_path.parent
    )
    os.close(handle)
    staging_path = Path(staging_name)
    try:
        torch.onnx.export(
            model,
            example_inputs,
            str(staging_path),
            input_names=list(input_names),
            output_names=list(output_names),
            dynamic_shapes=dynamic_shapes,
            opset_version=export_opset,
            external_data=False,
        )
        graph = onnx.load(str(staging_path), load_external_data=False)
        if export_opset != opset:
            graph = onnx.version_converter.convert_version(graph, opset)
            _prune_converted_attributes(graph.graph, export_opset, opset)
        if dynamic_axes is not None:
            _apply_axis_labels(graph.graph, _axis_labels(graph.graph, dynamic_axes))
        onnx.checker.check_model(graph)
        onnx.save(graph, str(staging_path), save_as_external_data=False)
        os.replace(staging_path, output_path)
    finally:
        staging_path.unlink(missing_ok=True)


__all__ = ["export_onnx"]
