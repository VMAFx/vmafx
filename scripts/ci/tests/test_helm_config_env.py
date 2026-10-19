#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The chart's VMAFX_* environment comes from _config.gen.tpl (ADR-2350 D13).

Every VMAFX_* entry of every workload is rendered by a `vmafx.env.<workload>`
helper of templates/_config.gen.tpl, which scripts/codegen/vmafx-api.py writes
from the [[chart_env]] entries of api/vmafx-platform.toml. Positive: no other
template names a VMAFX_* entry; the scoring server's container carries the
`env` values and nothing else. Planted change: an edited definition entry
changes the render, and only that entry. Negative: the server workloads set no
VMAFX_BACKEND, which vmafx-server does not read. Boundary: no `env` values
render no `env` list. The helm-chart workflow runs this file and needs
``helm`` on PATH.
"""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any

import tomllib
import yaml  # type: ignore[import-untyped]

ROOT = Path(__file__).resolve().parents[3]
CHART = ROOT / "deploy" / "helm" / "vmafx"
TEMPLATES = CHART / "templates"
GENERATED = TEMPLATES / "_config.gen.tpl"
DEFINITION = ROOT / "api" / "vmafx-platform.toml"
HELM = shutil.which("helm") or "helm"
TIMEOUT_S = 120
ENTRY = re.compile(r"^\s*- name: VMAFX_", re.MULTILINE)

sys.path.insert(0, str(ROOT / "scripts" / "codegen"))
from vmafx_api import emit_config  # noqa: E402 -- path set above
from vmafx_api.config import parse_config  # noqa: E402

API = ["--api-versions", "postgresql.cnpg.io/v1"]
PROVIDER_AUTH = """
controller: {enabled: true}
node: {enabled: true, controllerToken: {secretName: node-token}}
operator: {enabled: true, leaderElect: true, logLevel: debug, controllerToken: {secretName: op-token}}
auth: {enabled: true, issuer: "https://idp.example.com/", jwksEndpoint: "https://idp.example.com/keys",
       audience: vmafx, tenantClaim: org, rolesClaim: roles, scoringRoots: ["/data/{tenant}"]}
"""  # fmt: skip
WORKLOADS = ("Deployment", "Job", "StatefulSet")


def render(chart: Path, args: list[str], values: str) -> str:
    with tempfile.NamedTemporaryFile("w", suffix=".yaml") as fh:
        fh.write(values)
        fh.flush()
        done = subprocess.run(  # noqa: S603 -- fixed helm executable; argv built here
            [HELM, "template", "vmafx", str(chart), *API, "-f", fh.name, *args],
            capture_output=True,
            text=True,
            check=False,
            timeout=TIMEOUT_S,
        )
    return done.stdout + done.stderr


def server_container(workload: str, values: str) -> dict[str, Any]:
    """The vmafx-server container of `workload` rendered with `values`."""
    text = render(CHART, ["--set", f"workload={workload}"], values)
    for doc in yaml.safe_load_all(text):
        if doc and doc.get("kind") == workload and doc["metadata"]["name"] == "vmafx":
            pod = doc["spec"]["template"]["spec"]
            container: dict[str, Any] = pod["containers"][0]
            return container
    raise AssertionError(f"no {workload} in the render: {text[-400:]}")


class GeneratedEnvTest(unittest.TestCase):
    def test_no_other_template_writes_a_vmafx_entry(self) -> None:
        for path in sorted(TEMPLATES.rglob("*")):
            if path.is_file() and path != GENERATED:
                with self.subTest(path.name):
                    self.assertIsNone(ENTRY.search(path.read_text(encoding="utf-8")))
        self.assertGreater(len(ENTRY.findall(GENERATED.read_text(encoding="utf-8"))), 40)


@unittest.skipIf(shutil.which("helm") is None, "helm is not on PATH")
class RenderTest(unittest.TestCase):
    def test_a_definition_edit_changes_the_render(self) -> None:
        data = tomllib.loads(DEFINITION.read_text(encoding="utf-8"))
        entry = next(
            e
            for e in data["chart_env"]
            if e["env"] == "VMAFX_GRPC_LISTEN" and "node" in e["workloads"]
        )
        entry["value"] = '":{{ .Values.node.grpcPort | default 50052 }}0"'
        config = parse_config(data)
        assert config is not None
        with tempfile.TemporaryDirectory() as tmp:
            chart = Path(tmp) / "vmafx"
            shutil.copytree(CHART, chart)
            (chart / "templates" / GENERATED.name).write_text(emit_config.template_text(config))
            edited = render(chart, [], PROVIDER_AUTH).splitlines()
        before = render(CHART, [], PROVIDER_AUTH).splitlines()
        changed = [(a, b) for a, b in zip(before, edited, strict=True) if a != b]
        self.assertEqual(
            changed, [('              value: ":50052"', '              value: ":500520"')]
        )

    def test_the_server_container_carries_only_the_env_values(self) -> None:
        for workload in WORKLOADS:
            with self.subTest(workload):
                bare = server_container(workload, "")
                self.assertNotIn("env", bare)
                given = server_container(workload, "env: {VMAFX_LOG_LEVEL: debug}")
                self.assertEqual(given["env"], [{"name": "VMAFX_LOG_LEVEL", "value": "debug"}])
                vendor = server_container(workload, "gpu: {vendor: amd}")
                self.assertNotIn("env", vendor)


if __name__ == "__main__":
    unittest.main()
