#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The chart's values schema types Kubernetes fields as Kubernetes 1.26 does.

values.schema.json refers to the Kubernetes types of the minimum supported
minor for the values the chart copies into pod specs (ADR-2350 D13): the
tolerations, affinity, security contexts, probes, volumes, strategies,
environment sources, TLS entries, node selectors, labels and annotations.
Negative: each planted value Kubernetes refuses is refused at `helm template`,
naming its path; the schema before the Kubernetes types (BEFORE, the master
commit this change was built on) accepted every one of them when the clone
has that commit. Positive: valid values of the same fields render. Boundary:
the quantity and int-or-string forms Kubernetes reads as numbers render. The
helm-chart workflow runs this file and needs ``helm`` on PATH.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CHART = ROOT / "deploy" / "helm" / "vmafx"
BEFORE = "5d1ea07d9131bda8dd469d5156f1bf1ee9a66eb5"
HELM_TIMEOUT_S = 120
HELM = shutil.which("helm") or "helm"

sys.path.insert(0, str(ROOT / "scripts" / "codegen"))
from vmafx_api import gitref  # noqa: E402 -- path set above

# (label, values YAML, the path the refusal names)
REFUSED = [
    ("toleration seconds as text", "tolerations: [{key: gpu, tolerationSeconds: '60'}]",
     "/tolerations/0/tolerationSeconds"),
    ("node toleration as text", "node: {tolerations: [gpu]}", "/node/tolerations/0"),
    ("spread constraint without topologyKey",
     "topologySpreadConstraints: [{maxSkew: 1, whenUnsatisfiable: DoNotSchedule}]",
     "/topologySpreadConstraints/0"),
    ("pod user as a name", "podSecurityContext: {runAsUser: root}", "/podSecurityContext/runAsUser"),
    ("capabilities as text", "securityContext: {capabilities: {drop: ALL}}",
     "/securityContext/capabilities/drop"),
    ("volume without name", "node: {volumes: [{emptyDir: {}}]}", "/node/volumes/0"),
    ("mount without mountPath", "node: {volumeMounts: [{name: scratch}]}", "/node/volumeMounts/0"),
    ("probe period as text", "livenessProbe: {periodSeconds: often}", "/livenessProbe/periodSeconds"),
    ("affinity term as a list", "affinity: {nodeAffinity: [gpu]}", "/affinity/nodeAffinity"),
    ("node selector value as a number", "nodeSelector: {gpu: 1}", "/nodeSelector/gpu"),
    ("annotation value as a map", "podAnnotations: {team: {name: video}}", "/podAnnotations/team"),
    ("tls hosts as text", "ingress: {tls: [{hosts: vmafx.local}]}", "/ingress/tls/0/hosts"),
    ("strategy max surge as a list", "deployment: {strategy: {rollingUpdate: {maxSurge: [1]}}}",
     "/deployment/strategy/rollingUpdate/maxSurge"),
    ("env source as text", "envFrom: [config]", "/envFrom/0"),
]  # fmt: skip

ACCEPTED = [
    ("tolerations", "tolerations: [{key: nvidia.com/gpu, operator: Exists, effect: NoSchedule, "
     "tolerationSeconds: 300}]"),
    ("affinity", "affinity: {nodeAffinity: {requiredDuringSchedulingIgnoredDuringExecution: "
     "{nodeSelectorTerms: [{matchExpressions: [{key: gpu, operator: In, values: [nvidia]}]}]}}}"),
    ("spread", "topologySpreadConstraints: [{maxSkew: 1, topologyKey: kubernetes.io/hostname, "
     "whenUnsatisfiable: ScheduleAnyway}]"),
    ("volumes", "node: {volumes: [{name: scratch, emptyDir: {}}], volumeMounts: "
     "[{name: scratch, mountPath: /scratch}]}"),
    ("tls", "ingress: {tls: [{secretName: vmafx-tls, hosts: [vmafx.local]}]}"),
    ("env source", "envFrom: [{configMapRef: {name: extra}}]"),
    ("quantities as numbers", "resources: {limits: {cpu: 1.5, memory: 268435456}}"),
    ("ports as numbers or names", "livenessProbe: {httpGet: {path: /healthz, port: 8080}}\n"
     "readinessProbe: {httpGet: {path: /healthz, port: http}}"),
    ("surge as percent", "deployment: {strategy: {rollingUpdate: {maxSurge: 25%}}}"),
]  # fmt: skip


def render(chart: Path, values: str) -> subprocess.CompletedProcess[str]:
    with tempfile.NamedTemporaryFile("w", suffix=".yaml") as fh:
        fh.write(values + "\n")
        fh.flush()
        return subprocess.run(  # noqa: S603 -- fixed helm executable; argv built here
            [HELM, "template", "vmafx", str(chart), "-f", fh.name],
            capture_output=True,
            text=True,
            check=False,
            timeout=HELM_TIMEOUT_S,
        )


@unittest.skipIf(shutil.which("helm") is None, "helm is not on PATH")
class ValuesSchemaTest(unittest.TestCase):
    def test_planted_kubernetes_values_are_refused(self) -> None:
        for label, values, where in REFUSED:
            with self.subTest(label):
                result = render(CHART, values)
                self.assertNotEqual(result.returncode, 0, result.stdout[:200])
                self.assertIn(f"at '{where}", result.stderr)

    def test_the_schema_before_accepted_them(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            try:
                before = gitref.tree_at(ROOT, BEFORE, "deploy/helm/vmafx", Path(tmp))
            except gitref.Unavailable as exc:
                self.skipTest(f"the clone has no commit {BEFORE}: {exc}")
            for label, values, _ in REFUSED:
                with self.subTest(label):
                    result = render(before, values)
                    self.assertEqual(result.returncode, 0, result.stderr)

    def test_valid_kubernetes_values_render(self) -> None:
        for label, values in ACCEPTED:
            with self.subTest(label):
                result = render(CHART, values)
                self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
