# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Iterative tree mapping shared by the JSON-shaped helpers of the AI scripts."""

from __future__ import annotations

from collections.abc import Callable, Iterable
from typing import Any


def _never(_value: Any) -> bool:
    """Predicate that matches nothing."""
    return False


def _identity_key(key: Any) -> Any:
    """Return ``key`` unchanged."""
    return key


def _as_is(items: Iterable[tuple[Any, Any]]) -> Iterable[tuple[Any, Any]]:
    """Return the mapping items in their own order."""
    return items


def map_tree(
    value: Any,
    leaf: Callable[[Any], Any],
    *,
    is_mapping: Callable[[Any], bool] = _never,
    is_sequence: Callable[[Any], bool] = _never,
    key: Callable[[Any], Any] = _identity_key,
    order: Callable[[Iterable[tuple[Any, Any]]], Iterable[tuple[Any, Any]]] = _as_is,
) -> Any:
    """Rebuild ``value`` with ``leaf`` applied to every non-container node.

    Mappings become ``dict`` (keys through ``key``, items through ``order``) and
    sequences become ``list``.  The walk uses an explicit stack, so depth is not
    bounded by the interpreter recursion limit, and child order is preserved.
    """

    def shell(node: Any) -> Any:
        """Return an empty container for ``node``, or None for a leaf."""
        if is_mapping(node):
            return {}
        if is_sequence(node):
            return []
        return None

    root = shell(value)
    if root is None:
        return leaf(value)
    stack: list[tuple[Any, Any]] = [(value, root)]
    while stack:
        src, dst = stack.pop()
        pairs = order(src.items()) if is_mapping(src) else enumerate(src)
        for k, child in pairs:
            sub = shell(child)
            mapped = leaf(child) if sub is None else sub
            if sub is not None:
                stack.append((child, sub))
            if isinstance(dst, dict):
                dst[key(k)] = mapped
            else:
                dst.append(mapped)
    return root
