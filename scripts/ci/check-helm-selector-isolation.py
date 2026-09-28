#!/usr/bin/env python3
"""Check that every selector in a rendered Helm chart picks only its own Pods.

Copyright 2026 Lusoris
SPDX-License-Identifier: EUPL-1.2

Kubernetes does not stop two controllers from selecting the same Pods, and
their behaviour is then undefined. The chart's server Deployment selected only
the release labels (name + instance), so it also matched the operator and node
Pods: ``kubectl logs deployment/vmafx`` printed the operator's log. ADR-1353
added ``app.kubernetes.io/component: server`` to the server selectors; this
check keeps every component's selector disjoint from the others.

Input is the output of ``helm template`` (one or more files). The rules:

* A Deployment, StatefulSet, DaemonSet or ReplicaSet selector must match its
  own Pod template and no other Pod the chart renders (workload Pod templates,
  Job and CronJob templates, and bare Pods such as ``helm test`` hooks).
* A Service or PodDisruptionBudget selector may match the Pods of at most one
  of those sources. A Service without a selector is skipped: its endpoints are
  managed by hand.
* NetworkPolicies are not checked. The chart's default-deny policies select
  every Pod of the release on purpose.

Only ``matchLabels`` is evaluated. A selector with ``matchExpressions`` fails
the check instead of passing unevaluated; so does an empty workload selector.

Usage: check-helm-selector-isolation.py RENDERED.yaml [RENDERED.yaml ...]
Exit: 0 isolated, 1 an overlap or a selector the check cannot evaluate,
2 unreadable or unparsable input.
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import yaml  # type: ignore[import-untyped]

CONTROLLER_KINDS = frozenset({"Deployment", "StatefulSet", "DaemonSet", "ReplicaSet"})
CONSUMER_KINDS = frozenset({"Service", "PodDisruptionBudget"})

Manifest = dict[str, Any]


class SelectorError(ValueError):
    """A selector this check cannot evaluate exactly."""


@dataclass(frozen=True)
class PodSource:
    """Pods one rendered object creates, and the selector it owns them by."""

    ref: str
    namespace: str
    labels: dict[str, str]
    selector: dict[str, str] | None


def _ref(doc: Manifest) -> str:
    return f"{doc.get('kind')}/{(doc.get('metadata') or {}).get('name')}"


def _namespace(doc: Manifest) -> str:
    return str((doc.get("metadata") or {}).get("namespace") or "")


def _match_labels(selector: object, ref: str) -> dict[str, str]:
    """The equality requirements of a LabelSelector, or SelectorError."""
    if selector is None:
        return {}
    if not isinstance(selector, dict):
        raise SelectorError(f"{ref}: selector is not a mapping")
    if selector.get("matchExpressions"):
        raise SelectorError(f"{ref}: matchExpressions cannot be evaluated by this check")
    labels = selector.get("matchLabels") or {}
    return {str(key): str(value) for key, value in labels.items()}


def _template_labels(doc: Manifest) -> dict[str, str] | None:
    """Labels of the Pods a rendered object creates, or None if it creates none."""
    kind = doc.get("kind")
    spec = doc.get("spec") or {}
    if kind == "Pod":
        metadata = doc.get("metadata") or {}
    elif kind == "CronJob":
        job_spec = (spec.get("jobTemplate") or {}).get("spec") or {}
        metadata = (job_spec.get("template") or {}).get("metadata") or {}
    elif kind in CONTROLLER_KINDS or kind == "Job":
        metadata = (spec.get("template") or {}).get("metadata") or {}
    else:
        return None
    return {str(key): str(value) for key, value in (metadata.get("labels") or {}).items()}


def pod_sources(docs: list[Manifest]) -> list[PodSource]:
    """Every rendered object that creates Pods."""
    sources: list[PodSource] = []
    for doc in docs:
        labels = _template_labels(doc)
        if labels is None:
            continue
        selector = None
        if doc.get("kind") in CONTROLLER_KINDS:
            selector = _match_labels((doc.get("spec") or {}).get("selector"), _ref(doc))
        sources.append(PodSource(_ref(doc), _namespace(doc), labels, selector))
    return sources


def _matches(selector: dict[str, str], labels: dict[str, str]) -> bool:
    return all(labels.get(key) == value for key, value in selector.items())


def _selected(selector: dict[str, str], namespace: str, sources: list[PodSource]) -> list[str]:
    return [
        source.ref
        for source in sources
        if source.namespace == namespace and _matches(selector, source.labels)
    ]


def controller_errors(sources: list[PodSource]) -> list[str]:
    """Controllers whose selector is empty, misses their Pods, or takes others'."""
    errors = []
    for source in sources:
        if source.selector is None:
            continue
        if not source.selector:
            errors.append(f"{source.ref}: empty selector matches every Pod in the namespace")
            continue
        if not _matches(source.selector, source.labels):
            errors.append(f"{source.ref}: selector {source.selector} misses its own Pod template")
        others = [
            ref
            for ref in _selected(source.selector, source.namespace, sources)
            if ref != source.ref
        ]
        if others:
            errors.append(
                f"{source.ref}: selector {source.selector} also matches Pods of "
                + ", ".join(others)
            )
    return errors


def consumer_errors(docs: list[Manifest], sources: list[PodSource]) -> list[str]:
    """Services and PodDisruptionBudgets that select Pods of several sources."""
    errors = []
    for doc in docs:
        kind = doc.get("kind")
        if kind not in CONSUMER_KINDS:
            continue
        spec = doc.get("spec") or {}
        if kind == "Service":
            selector = {str(key): str(value) for key, value in (spec.get("selector") or {}).items()}
            if not selector:
                continue
        else:
            selector = _match_labels(spec.get("selector"), _ref(doc))
        selected = _selected(selector, _namespace(doc), sources)
        if len(selected) > 1:
            errors.append(
                f"{_ref(doc)}: selector {selector} matches Pods of " + ", ".join(selected)
            )
    return errors


def isolation_errors(docs: list[Manifest]) -> list[str]:
    """Every selector overlap in the rendered documents; empty when isolated."""
    try:
        sources = pod_sources(docs)
        return controller_errors(sources) + consumer_errors(docs, sources)
    except SelectorError as error:
        return [str(error)]


def load_documents(paths: list[Path]) -> list[Manifest]:
    """Every mapping document in the given multi-document YAML files."""
    docs: list[Manifest] = []
    for path in paths:
        with path.open(encoding="utf-8") as handle:
            docs.extend(doc for doc in yaml.safe_load_all(handle) if isinstance(doc, dict))
    return docs


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Check that rendered Helm selectors pick only their own Pods."
    )
    parser.add_argument("rendered", nargs="+", type=Path, help="output of helm template")
    args = parser.parse_args(argv)
    try:
        docs = load_documents(args.rendered)
    except (OSError, yaml.YAMLError) as error:
        print(f"error: cannot read rendered chart: {error}", file=sys.stderr)
        return 2
    errors = isolation_errors(docs)
    for message in errors:
        print(f"error: {message}", file=sys.stderr)
    if errors:
        return 1
    print(f"selector isolation: {len(pod_sources(docs))} Pod sources, no overlap")
    return 0


if __name__ == "__main__":
    sys.exit(main())
