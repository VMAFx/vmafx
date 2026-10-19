#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The monitoring part of the Helm chart, checked on real renders (ADR-2349).

monitoring.enabled renders a ServiceMonitor per deployed component (server,
controller, node) and a PodMonitor for the operator, the PrometheusRule of the
generated rules with the SLO settings from the values, one ConfigMap per
generated dashboard for the Grafana sidecar, and, with networkPolicy.enabled,
the ingress Prometheus needs. The PrometheusRule rendered with the defaults
has the groups of deploy/prometheus/vmafx-rules.yaml; rendered with an
override it has the groups `go run ./tools/obsgen -render-rules` writes for
the same file, and promtool accepts both. The schema refuses settings the
rules cannot use. Positive, negative and boundary renders; the helm-chart
workflow runs this file and needs ``helm`` and ``go`` on PATH.
"""

from __future__ import annotations

import json
import shutil
import subprocess
import tempfile
import unittest
from typing import Any

import yaml  # type: ignore[import-untyped]
from test_helm_controller_workload import CHART, named, render

ROOT = CHART.parents[2]
DEPLOYED = """
controller: {enabled: true}
node: {enabled: true}
operator: {enabled: true}
auth: {enabled: true, disabled: true}
"""
ALL_COMPONENTS = DEPLOYED + "monitoring: {enabled: true}\n"
OVERRIDE = """
monitoring:
  enabled: true
  slo: {jobSuccess: 0.995, scoreLatency: 0.999, scoreLatencySeconds: "0.25"}
  burnRates:
    fast: {longWindow: 2h, shortWindow: 10m, factor: 13.37}
    slow: {longWindow: 1d, shortWindow: 2h, factor: 3, for: 1h30m}
  alerts: {queueAgeSeconds: 1000000, scoreRegressionPoints: 2.5}
  cost: {perJobSecond: 0.002, perJob: 0.1, currency: EUR}
"""
GO = shutil.which("go") or "go"
BASH = shutil.which("bash") or "/bin/bash"


def kinds(docs: list[dict[str, Any]], kind: str) -> dict[str, dict[str, Any]]:
    return {d["metadata"]["name"]: d for d in docs if d["kind"] == kind}


def helm_fails(values: str) -> str:
    """Render values and return helm's error; fail when the render succeeds."""
    try:
        render(values)
    except AssertionError as err:
        return str(err)
    raise AssertionError(f"helm rendered values the schema must refuse:\n{values}")


def run(*argv: str) -> str:
    result = subprocess.run(  # noqa: S603 -- fixed go / bash executables; argv built here
        list(argv), cwd=ROOT, capture_output=True, text=True, check=False
    )
    if result.returncode != 0:
        raise AssertionError(f"{argv} failed: {result.stdout}{result.stderr}")
    return result.stdout


def promtool_accepts(groups: dict[str, Any]) -> None:
    promtool = run(BASH, "scripts/ci/pinned-tool.sh", "promtool").strip()
    with tempfile.NamedTemporaryFile("w", suffix=".yaml") as fh:
        yaml.safe_dump(groups, fh)
        fh.flush()
        run(promtool, "check", "rules", fh.name)


class HelmObservabilityTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        for tool in ("helm", "go"):
            if shutil.which(tool) is None:
                raise AssertionError(f"{tool} is not on PATH; this test needs it")

    def test_nothing_renders_while_monitoring_is_off(self) -> None:
        docs = render("node: {enabled: true}\noperator: {enabled: true}\n")
        for kind in ("ServiceMonitor", "PodMonitor", "PrometheusRule"):
            self.assertEqual(kinds(docs, kind), {}, kind)
        cms = [n for n in kinds(docs, "ConfigMap") if "-dashboard-" in n]
        self.assertEqual(cms, [])

    def test_a_monitor_per_deployed_component(self) -> None:
        docs = render(ALL_COMPONENTS)
        self.assertEqual(
            set(kinds(docs, "ServiceMonitor")),
            {"vmafx", "vmafx-controller", "vmafx-node"},
        )
        pod = named(docs, "PodMonitor", "vmafx-operator")
        endpoint = pod["spec"]["podMetricsEndpoints"][0]
        self.assertEqual((endpoint["port"], endpoint["path"]), ("metrics", "/metrics"))
        ports = {
            n: m["spec"]["endpoints"][0]["port"] for n, m in kinds(docs, "ServiceMonitor").items()
        }
        self.assertEqual(
            ports, {"vmafx": "http", "vmafx-controller": "http", "vmafx-node": "metrics"}
        )
        for name, monitor in kinds(docs, "ServiceMonitor").items():
            component = name.removeprefix("vmafx").removeprefix("-") or "server"
            selector = monitor["spec"]["selector"]
            self.assertEqual(selector["matchLabels"]["app.kubernetes.io/component"], component)
            self.assertIn(
                {"key": "vmafx.dev/headless", "operator": "DoesNotExist"},
                selector["matchExpressions"],
            )

    def test_components_switch_off_and_follow_deployment(self) -> None:
        docs = render(DEPLOYED + "monitoring: {enabled: true, components: {node: false}}\n")
        self.assertNotIn("vmafx-node", kinds(docs, "ServiceMonitor"))
        docs = render("monitoring: {enabled: true}\n")
        self.assertEqual(set(kinds(docs, "ServiceMonitor")), {"vmafx"})
        self.assertEqual(kinds(docs, "PodMonitor"), {})

    def test_monitor_in_another_namespace_selects_the_release(self) -> None:
        docs = render("monitoring: {enabled: true, serviceMonitor: {namespace: mon}}\n")
        monitor = named(docs, "ServiceMonitor", "vmafx")
        self.assertEqual(monitor["metadata"]["namespace"], "mon")
        self.assertEqual(monitor["spec"]["namespaceSelector"], {"matchNames": ["vmafx"]})
        docs = render("monitoring: {enabled: true, serviceMonitor: {namespace: vmafx}}\n")
        self.assertNotIn("namespaceSelector", named(docs, "ServiceMonitor", "vmafx")["spec"])

    def test_headless_service_is_not_scraped_twice(self) -> None:
        docs = render("workload: StatefulSet\nmonitoring: {enabled: true}\n")
        headless = named(docs, "Service", "vmafx-headless")
        self.assertEqual(headless["metadata"]["labels"]["vmafx.dev/headless"], "true")
        self.assertNotIn(
            "vmafx.dev/headless", named(docs, "Service", "vmafx")["metadata"]["labels"]
        )

    def test_node_metrics_port_moves_the_listener(self) -> None:
        docs = render("node: {enabled: true, metricsPort: 9100}\nmonitoring: {enabled: true}\n")
        node = named(docs, "Deployment", "vmafx-node")
        container = node["spec"]["template"]["spec"]["containers"][0]
        env = {e["name"]: e.get("value") for e in container["env"]}
        self.assertEqual(env["VMAFX_HTTP_ADDR"], ":9100")
        ports = {p["name"]: p["containerPort"] for p in container["ports"]}
        self.assertEqual(ports["metrics"], 9100)

    def test_default_rule_is_the_generated_rule_file(self) -> None:
        rule = named(render("monitoring: {enabled: true}\n"), "PrometheusRule", "vmafx")
        committed = yaml.safe_load((ROOT / "deploy/prometheus/vmafx-rules.yaml").read_text())
        self.assertEqual(rule["spec"], committed)
        promtool_accepts(rule["spec"])

    def test_overridden_settings_match_the_compose_renderer(self) -> None:
        rule = named(render(OVERRIDE), "PrometheusRule", "vmafx")
        with tempfile.NamedTemporaryFile("w", suffix=".yaml") as fh:
            fh.write(OVERRIDE)
            fh.flush()
            plain = yaml.safe_load(
                run(GO, "run", "./tools/obsgen", "-render-rules", "-values", fh.name)
            )
        self.assertEqual(rule["spec"], plain)
        exprs = [r["expr"] for g in plain["groups"] for r in g["rules"]]
        self.assertTrue(any("(13.37 * (1 - 0.999))" in e for e in exprs))
        self.assertTrue(any('le=~"0.25(\\\\.0)?"' in e for e in exprs))
        records = [r["record"] for r in plain["groups"][0]["rules"] if "record" in r]
        self.assertEqual(records.count("vmafx:job_failure_ratio:rate2h"), 1)
        prices = {
            r["record"]: r
            for r in plain["groups"][0]["rules"]
            if r.get("record", "").startswith("vmafx:price_")
        }
        self.assertEqual(prices["vmafx:price_job"]["labels"], {"currency": "EUR"})
        self.assertIn("+ 0.002) > 0", prices["vmafx:price_job_second"]["expr"])
        promtool_accepts(rule["spec"])

    def test_rule_switches_and_labels(self) -> None:
        docs = render("monitoring: {enabled: true, prometheusRule: {enabled: false}}\n")
        self.assertEqual(kinds(docs, "PrometheusRule"), {})
        docs = render(
            "monitoring: {enabled: true, prometheusRule: {namespace: mon, labels: {release: kps}}}\n"
        )
        meta = named(docs, "PrometheusRule", "vmafx")["metadata"]
        self.assertEqual((meta["namespace"], meta["labels"]["release"]), ("mon", "kps"))

    def test_schema_refuses_unusable_settings(self) -> None:
        for bad, field in (
            ("slo: {jobSuccess: 1}", "jobSuccess"),
            ("slo: {scoreLatency: 0}", "scoreLatency"),
            ('slo: {scoreLatencySeconds: "31"}', "scoreLatencySeconds"),
            ("slo: {scoreLatencySeconds: 30}", "scoreLatencySeconds"),
            ("burnRates: {fast: {longWindow: 1x}}", "longWindow"),
            ("burnRates: {slow: {factor: 0}}", "factor"),
            ("alerts: {queueAgeSeconds: 0}", "queueAgeSeconds"),
            ("alerts: {scoreRegressionPoints: 101}", "scoreRegressionPoints"),
            ("slo: {objective: 0.9}", "objective"),
        ):
            with self.subTest(bad=bad):
                self.assertIn(field, helm_fails(f"monitoring: {{enabled: true, {bad}}}\n"))

    def test_dashboards_are_the_generated_files(self) -> None:
        docs = render("monitoring: {enabled: true}\n")
        cms = {n: d for n, d in kinds(docs, "ConfigMap").items() if "-dashboard-" in n}
        shipped = sorted((ROOT / "deploy/grafana/dashboards").glob("*.json"))
        vmafx = [p for p in shipped if not p.name.startswith("vmafx-gpu-")]
        self.assertEqual(len(cms), len(vmafx))
        for path in vmafx:
            cm = cms["vmafx-dashboard-" + path.stem.removeprefix("vmafx-")]
            self.assertEqual(cm["metadata"]["labels"]["grafana_dashboard"], "1")
            self.assertEqual(json.loads(cm["data"][path.name]), json.loads(path.read_text()))

    def test_gpu_exporter_dashboards_are_opt_in(self) -> None:
        docs = render(
            "monitoring: {enabled: true, dashboards: {gpuExporters: {amd: true}, "
            "annotations: {grafana_folder: VMAFx}}}\n"
        )
        cms = {n for n in kinds(docs, "ConfigMap") if "-dashboard-gpu-" in n}
        self.assertEqual(cms, {"vmafx-dashboard-gpu-amd"})
        cm = named(docs, "ConfigMap", "vmafx-dashboard-gpu-amd")
        self.assertEqual(cm["metadata"]["annotations"], {"grafana_folder": "VMAFx"})
        docs = render("monitoring: {enabled: true, dashboards: {enabled: false}}\n")
        self.assertEqual([n for n in kinds(docs, "ConfigMap") if "-dashboard-" in n], [])

    def test_network_policy_admits_the_scrape(self) -> None:
        values = ALL_COMPONENTS + "networkPolicy: {enabled: true}\n"
        policy = named(render(values), "NetworkPolicy", "vmafx-allow-metrics-scrape")
        ingress = policy["spec"]["ingress"][0]
        self.assertEqual(ingress["from"], [{"podSelector": {}}])
        self.assertEqual(sorted(p["port"] for p in ingress["ports"]), [8080, 9090])
        values = ALL_COMPONENTS + (
            "networkPolicy: {enabled: true, allow: {metricsScrape: "
            "{fromNamespaceSelector: {team: mon}}}}\n"
        )
        policy = named(render(values), "NetworkPolicy", "vmafx-allow-metrics-scrape")
        self.assertEqual(
            policy["spec"]["ingress"][0]["from"],
            [{"podSelector": {}, "namespaceSelector": {"matchLabels": {"team": "mon"}}}],
        )
        docs = render("networkPolicy: {enabled: true}\n")
        self.assertNotIn("vmafx-allow-metrics-scrape", kinds(docs, "NetworkPolicy"))


if __name__ == "__main__":
    unittest.main()
