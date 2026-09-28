#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Positive, negative and boundary cases for check-helm-selector-isolation.py.

The manifests are synthetic and shaped like the chart's own output. The
negative cases include the v1.0.0-rc.1 server Deployment, whose selector held
only the release labels and so matched the operator and node Pods (ADR-1353).
The helm-chart workflow runs the same check against real `helm template`
renders of the chart.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any

import yaml  # type: ignore[import-untyped]

GATE = Path(__file__).resolve().parents[1] / "check-helm-selector-isolation.py"
_SPEC = importlib.util.spec_from_file_location("check_helm_selector_isolation", GATE)
assert _SPEC is not None and _SPEC.loader is not None
gate = importlib.util.module_from_spec(_SPEC)
sys.modules[_SPEC.name] = gate
_SPEC.loader.exec_module(gate)

RELEASE = {"app.kubernetes.io/name": "vmafx", "app.kubernetes.io/instance": "vmafx"}


def labels(component: str | None = None, **extra: str) -> dict[str, str]:
    result = dict(RELEASE)
    if component is not None:
        result["app.kubernetes.io/component"] = component
    result.update(extra)
    return result


def workload(
    kind: str,
    name: str,
    selector: dict[str, Any] | None,
    pod_labels: dict[str, str],
    namespace: str = "vmafx",
) -> dict[str, Any]:
    spec: dict[str, Any] = {"template": {"metadata": {"labels": pod_labels}}}
    if selector is not None:
        spec["selector"] = selector
    return {
        "kind": kind,
        "metadata": {"name": name, "namespace": namespace},
        "spec": spec,
    }


def component_deployment(name: str, component: str) -> dict[str, Any]:
    return workload("Deployment", name, {"matchLabels": labels(component)}, labels(component))


def service(name: str, selector: dict[str, str] | None) -> dict[str, Any]:
    spec = {} if selector is None else {"selector": selector}
    return {"kind": "Service", "metadata": {"name": name, "namespace": "vmafx"}, "spec": spec}


def pdb(name: str, component: str) -> dict[str, Any]:
    return {
        "kind": "PodDisruptionBudget",
        "metadata": {"name": name, "namespace": "vmafx"},
        "spec": {"selector": {"matchLabels": labels(component)}},
    }


def chart(server: dict[str, Any]) -> list[dict[str, Any]]:
    """The Deployment-mode render with operator, node and helm test enabled."""
    test_pod = {
        "kind": "Pod",
        "metadata": {
            "name": "vmafx-test-connection",
            "namespace": "vmafx",
            "labels": labels(**{"helm.sh/chart-test": "true"}),
        },
    }
    return [
        server,
        component_deployment("vmafx-operator", "operator"),
        component_deployment("vmafx-node", "node"),
        service("vmafx", labels("server")),
        service("vmafx-node", labels("node")),
        pdb("vmafx", "server"),
        pdb("vmafx-node", "node"),
        test_pod,
    ]


class IsolatedSelectors(unittest.TestCase):
    """Positive cases: renders the check must accept."""

    def test_component_selectors_are_disjoint(self) -> None:
        docs = chart(component_deployment("vmafx", "server"))
        self.assertEqual(gate.isolation_errors(docs), [])

    def test_statefulset_with_headless_service(self) -> None:
        server = workload(
            "StatefulSet", "vmafx", {"matchLabels": labels("server")}, labels("server")
        )
        docs = [*chart(server), service("vmafx-headless", labels("server"))]
        self.assertEqual(gate.isolation_errors(docs), [])

    def test_job_pods_have_no_selector_to_check(self) -> None:
        job = workload("Job", "vmafx", None, labels("server"))
        docs = [job, component_deployment("vmafx-operator", "operator")]
        docs.append(service("vmafx", labels("server")))
        self.assertEqual(gate.isolation_errors(docs), [])

    def test_cli_accepts_isolated_render(self) -> None:
        docs = chart(component_deployment("vmafx", "server"))
        code, output = run_cli(yaml.safe_dump_all(docs))
        self.assertEqual(code, 0, output)
        self.assertIn("no overlap", output)


class OverlappingSelectors(unittest.TestCase):
    """Negative cases: overlaps the check must reject, naming both sides."""

    def test_rc1_server_deployment_selector_is_rejected(self) -> None:
        rc1 = workload("Deployment", "vmafx", {"matchLabels": labels()}, labels("server"))
        errors = gate.isolation_errors(chart(rc1))
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("Deployment/vmafx: selector", errors[0])
        for other in (
            "Deployment/vmafx-operator",
            "Deployment/vmafx-node",
            "Pod/vmafx-test-connection",
        ):
            self.assertIn(other, errors[0])

    def test_rc1_server_statefulset_selector_is_rejected(self) -> None:
        rc1 = workload("StatefulSet", "vmafx", {"matchLabels": labels()}, labels("server"))
        errors = gate.isolation_errors(chart(rc1))
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("StatefulSet/vmafx", errors[0])
        self.assertIn("Deployment/vmafx-operator", errors[0])

    def test_service_spanning_components_is_rejected(self) -> None:
        docs = chart(component_deployment("vmafx", "server"))
        docs.append(service("vmafx-broad", labels()))
        errors = gate.isolation_errors(docs)
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("Service/vmafx-broad", errors[0])

    def test_pdb_spanning_components_is_rejected(self) -> None:
        docs = chart(component_deployment("vmafx", "server"))
        docs.append(
            {
                "kind": "PodDisruptionBudget",
                "metadata": {"name": "vmafx-all", "namespace": "vmafx"},
                "spec": {"selector": {"matchLabels": labels()}},
            }
        )
        errors = gate.isolation_errors(docs)
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("PodDisruptionBudget/vmafx-all", errors[0])

    def test_selector_missing_its_own_pods_is_rejected(self) -> None:
        server = workload("Deployment", "vmafx", {"matchLabels": labels("server")}, labels("api"))
        errors = gate.isolation_errors([server])
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("misses its own Pod template", errors[0])

    def test_cronjob_pods_count_as_other_pods(self) -> None:
        cronjob = {
            "kind": "CronJob",
            "metadata": {"name": "vmafx-gc", "namespace": "vmafx"},
            "spec": {"jobTemplate": {"spec": {"template": {"metadata": {"labels": labels("gc")}}}}},
        }
        broad = workload("Deployment", "vmafx", {"matchLabels": labels()}, labels("server"))
        errors = gate.isolation_errors([broad, cronjob])
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("CronJob/vmafx-gc", errors[0])

    def test_cli_rejects_overlap(self) -> None:
        rc1 = workload("Deployment", "vmafx", {"matchLabels": labels()}, labels("server"))
        code, output = run_cli(yaml.safe_dump_all(chart(rc1)))
        self.assertEqual(code, 1, output)
        self.assertIn("error: Deployment/vmafx: selector", output)


class SelectorBoundaries(unittest.TestCase):
    """Boundary cases: exact matches, empty selectors and unreadable input."""

    def test_selector_equal_to_another_pods_full_label_set_is_rejected(self) -> None:
        other = labels("operator")
        broad = workload("Deployment", "vmafx", {"matchLabels": other}, labels("server"))
        errors = gate.isolation_errors([broad, component_deployment("vmafx-operator", "operator")])
        self.assertTrue(any("misses its own Pod template" in error for error in errors), errors)
        self.assertTrue(any("also matches Pods of" in error for error in errors), errors)

    def test_one_differing_label_value_is_enough(self) -> None:
        docs = [
            component_deployment("vmafx", "server"),
            component_deployment("vmafx-servers", "server-canary"),
        ]
        self.assertEqual(gate.isolation_errors(docs), [])

    def test_empty_selector_is_rejected(self) -> None:
        empty = workload("Deployment", "vmafx", {"matchLabels": {}}, labels("server"))
        errors = gate.isolation_errors([empty])
        self.assertEqual(
            errors, ["Deployment/vmafx: empty selector matches every Pod in the namespace"]
        )

    def test_match_expressions_fail_closed(self) -> None:
        selector = {
            "matchLabels": labels("server"),
            "matchExpressions": [
                {"key": "app.kubernetes.io/component", "operator": "In", "values": ["server"]}
            ],
        }
        server = workload("Deployment", "vmafx", selector, labels("server"))
        errors = gate.isolation_errors([server])
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("matchExpressions cannot be evaluated", errors[0])

    def test_other_namespaces_do_not_overlap(self) -> None:
        here = workload("Deployment", "vmafx", {"matchLabels": labels()}, labels("server"))
        there = workload(
            "Deployment",
            "vmafx-operator",
            {"matchLabels": labels("operator")},
            labels("operator"),
            namespace="elsewhere",
        )
        self.assertEqual(gate.isolation_errors([here, there]), [])

    def test_service_without_selector_is_skipped(self) -> None:
        docs = [*chart(component_deployment("vmafx", "server")), service("vmafx-manual", None)]
        self.assertEqual(gate.isolation_errors(docs), [])

    def test_cli_reports_unparsable_input(self) -> None:
        code, output = run_cli("kind: [unclosed\n")
        self.assertEqual(code, 2, output)
        self.assertIn("cannot read rendered chart", output)

    def test_cli_reports_missing_file(self) -> None:
        code, output = run_cli(None)
        self.assertEqual(code, 2, output)
        self.assertIn("cannot read rendered chart", output)


def run_cli(text: str | None) -> tuple[int, str]:
    """Run the CLI on one rendered file; None names a file that does not exist."""
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "rendered.yaml"
        if text is not None:
            path.write_text(text, encoding="utf-8")
        output = io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            code = gate.main([str(path)])
        return code, output.getvalue()


if __name__ == "__main__":
    unittest.main()
