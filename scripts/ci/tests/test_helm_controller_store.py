#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Where the chart's vmafx-controller keeps its jobs, checked on real renders.

ADR-2350: controller.store.backend sqlite keeps today's one-replica workload
(Recreate, the queue on a ReadWriteOnce claim); postgres runs any number of
replicas with a rolling update and no claim, on a CloudNativePG Cluster the
chart renders (the operator is a prerequisite) or on an external database
named by a Secret. A migration Job applies the schema; PodDisruptionBudget,
topology spread and NetworkPolicies follow the replica count and the backend.
Positive, negative and boundary renders; needs ``helm`` on PATH.
"""

from __future__ import annotations

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import Any

import yaml  # type: ignore[import-untyped]

CHART = Path(__file__).resolve().parents[3] / "deploy" / "helm" / "vmafx"
CNPG_API = "postgresql.cnpg.io/v1"
BASE = """
controller: {enabled: true}
auth: {enabled: true, disabled: true}
"""
POSTGRES = """
controller:
  enabled: true
  replicas: 2
  store: {backend: postgres}
auth: {enabled: true, disabled: true}
"""


def helm_template(values: str, *args: str) -> subprocess.CompletedProcess[str]:
    """Run helm template in namespace vmafx with an extra values file."""
    with tempfile.NamedTemporaryFile("w", suffix=".yaml") as fh:
        fh.write(values)
        fh.flush()
        cmd = [
            "helm",
            "template",
            "vmafx",
            str(CHART),
            "--namespace",
            "vmafx",
            "-f",
            fh.name,
            *args,
        ]
        return subprocess.run(  # noqa: S603 -- fixed helm executable; argv built here
            cmd, capture_output=True, text=True, check=False
        )


def render(values: str, *args: str) -> list[dict[str, Any]]:
    result = helm_template(values, *args)
    if result.returncode != 0:
        raise AssertionError(f"helm template failed: {result.stderr}")
    return [doc for doc in yaml.safe_load_all(result.stdout) if doc]


def refused(values: str, *args: str) -> str:
    result = helm_template(values, *args)
    if result.returncode == 0:
        raise AssertionError("helm template rendered a configuration the chart must refuse")
    return result.stderr


def of_kind(docs: list[dict[str, Any]], kind: str) -> list[dict[str, Any]]:
    return [d for d in docs if d["kind"] == kind]


def named(docs: list[dict[str, Any]], kind: str, name: str) -> dict[str, Any]:
    return next(d for d in docs if d["kind"] == kind and d["metadata"]["name"] == name)


def container(pod_owner: dict[str, Any]) -> dict[str, Any]:
    c: dict[str, Any] = pod_owner["spec"]["template"]["spec"]["containers"][0]
    return c


def env(pod_owner: dict[str, Any]) -> dict[str, Any]:
    return {
        e["name"]: e.get("value", e.get("valueFrom")) for e in container(pod_owner).get("env", [])
    }


def controller(docs: list[dict[str, Any]]) -> dict[str, Any]:
    return named(docs, "Deployment", "vmafx-controller")


class HelmControllerStoreTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if shutil.which("helm") is None:
            raise AssertionError("helm is not on PATH; this test renders the chart")

    def test_sqlite_stays_one_replica_on_its_claim(self) -> None:
        docs = render(BASE)
        dep = controller(docs)
        self.assertEqual(dep["spec"]["replicas"], 1)
        self.assertEqual(dep["spec"]["strategy"]["type"], "Recreate")
        self.assertEqual(env(dep)["VMAFX_DB_PATH"], "/data/vmafx-controller.db")
        self.assertNotIn("VMAFX_DB_DSN", env(dep))
        self.assertEqual(len(of_kind(docs, "PersistentVolumeClaim")), 1)
        self.assertEqual(of_kind(docs, "Cluster"), [])
        self.assertEqual(of_kind(docs, "Job"), [])

    def test_postgres_on_cloudnativepg(self) -> None:
        docs = render(POSTGRES, "--api-versions", CNPG_API)
        dep = controller(docs)
        self.assertEqual(dep["spec"]["replicas"], 2)
        self.assertEqual(dep["spec"]["strategy"]["type"], "RollingUpdate")
        e = env(dep)
        self.assertEqual(e["VMAFX_STORE_BACKEND"], "postgres")
        self.assertEqual(
            e["VMAFX_DB_DSN"], {"secretKeyRef": {"name": "vmafx-db-app", "key": "uri"}}
        )
        self.assertNotIn("VMAFX_DB_PATH", e)
        self.assertEqual(of_kind(docs, "PersistentVolumeClaim"), [])
        mounts = {m["mountPath"] for m in container(dep).get("volumeMounts", [])}
        self.assertNotIn("/data", mounts)
        cluster = named(docs, "Cluster", "vmafx-db")
        self.assertEqual(cluster["apiVersion"], CNPG_API)
        self.assertEqual(cluster["spec"]["instances"], 1)
        self.assertRegex(
            cluster["spec"]["imageName"],
            r"^ghcr\.io/cloudnative-pg/postgresql:18\.\S+@sha256:[0-9a-f]{64}$",
        )
        self.assertEqual(
            cluster["spec"]["bootstrap"]["initdb"], {"database": "vmafx", "owner": "vmafx"}
        )

    def test_migration_job_runs_migrate_with_the_controller_image(self) -> None:
        docs = render(POSTGRES, "--api-versions", CNPG_API)
        jobs = of_kind(docs, "Job")
        self.assertEqual(len(jobs), 1)
        job = jobs[0]
        self.assertRegex(job["metadata"]["name"], r"^vmafx-controller-migrate-[0-9a-f]{8}$")
        c = container(job)
        self.assertEqual(c["image"], container(controller(docs))["image"])
        self.assertEqual(c["args"], ["migrate"])
        self.assertEqual(
            env(job)["VMAFX_DB_DSN"], {"secretKeyRef": {"name": "vmafx-db-app", "key": "uri"}}
        )
        self.assertEqual(job["spec"]["template"]["spec"]["restartPolicy"], "OnFailure")
        # A Job's pod template is immutable: the name follows the template, so
        # an unchanged release keeps the completed Job and a change makes a new one.
        name = job["metadata"]["name"]
        again = render(POSTGRES, "--api-versions", CNPG_API)
        self.assertEqual(of_kind(again, "Job")[0]["metadata"]["name"], name)
        for change in ("controller.image.tag=v9.9.9", "controller.resources.limits.cpu=3"):
            with self.subTest(change=change):
                other = render(POSTGRES, "--api-versions", CNPG_API, "--set", change)
                self.assertNotEqual(of_kind(other, "Job")[0]["metadata"]["name"], name)

    def test_cloudnativepg_is_a_prerequisite(self) -> None:
        self.assertIn("CloudNativePG", refused(POSTGRES))

    def test_external_database_from_a_secret(self) -> None:
        external = ("--set", "controller.store.postgresql.mode=external")
        external_keys = "controller.store.postgresql.external"
        docs = render(
            POSTGRES,
            *external,
            "--set",
            "controller.replicas=3",
            "--set",
            f"{external_keys}.secretName=my-db",
            "--set",
            f"{external_keys}.secretKey=dsn",
        )
        e = env(controller(docs))
        self.assertEqual(e["VMAFX_DB_DSN"], {"secretKeyRef": {"name": "my-db", "key": "dsn"}})
        self.assertEqual(of_kind(docs, "Cluster"), [])
        self.assertEqual(controller(docs)["spec"]["replicas"], 3)
        self.assertIn("secretName", refused(POSTGRES, *external))

    def test_several_replicas_need_postgres(self) -> None:
        self.assertIn("postgres", refused(BASE, "--set", "controller.replicas=2"))
        zero = ("--set", "controller.replicas=0")
        self.assertIn("replicas", refused(POSTGRES, "--api-versions", CNPG_API, *zero))
        self.assertIn("backend", refused(BASE, "--set", "controller.store.backend=etcd"))

    def test_store_lifetimes_reach_the_controller_only_when_set(self) -> None:
        docs = render(POSTGRES, "--api-versions", CNPG_API)
        self.assertFalse(
            [
                k
                for k in env(controller(docs))
                if k.startswith("VMAFX_STORE_") and k != "VMAFX_STORE_BACKEND"
            ]
        )
        lifetimes = {
            "leaseTTL": "90s",
            "sessionTTL": "2m",
            "sweepInterval": "3s",
            "backoffBase": "1s",
            "backoffMax": "1m",
        }
        for key in lifetimes:
            with self.subTest(refused=key):
                bad = ("--set", f"controller.store.{key}=soon")
                self.assertIn(key, refused(POSTGRES, "--api-versions", CNPG_API, *bad))
        sets = [a for k, v in lifetimes.items() for a in ("--set", f"controller.store.{k}={v}")]
        e = env(controller(render(POSTGRES, "--api-versions", CNPG_API, *sets)))
        self.assertEqual(
            {k: e[k] for k in e if k.startswith("VMAFX_STORE_")},
            {
                "VMAFX_STORE_BACKEND": "postgres",
                "VMAFX_STORE_LEASE_TTL": "90s",
                "VMAFX_STORE_SESSION_TTL": "2m",
                "VMAFX_STORE_SWEEP_INTERVAL": "3s",
                "VMAFX_STORE_BACKOFF_BASE": "1s",
                "VMAFX_STORE_BACKOFF_MAX": "1m",
            },
        )

    def test_replicas_bring_a_disruption_budget_and_a_spread(self) -> None:
        docs = render(POSTGRES, "--api-versions", CNPG_API)
        pdb = named(docs, "PodDisruptionBudget", "vmafx-controller")
        self.assertEqual(pdb["spec"]["maxUnavailable"], 1)
        self.assertEqual(
            pdb["spec"]["selector"]["matchLabels"]["app.kubernetes.io/component"], "controller"
        )
        spread = controller(docs)["spec"]["template"]["spec"]["topologySpreadConstraints"]
        self.assertEqual(spread[0]["topologyKey"], "kubernetes.io/hostname")
        self.assertEqual(spread[0]["whenUnsatisfiable"], "ScheduleAnyway")
        self.assertEqual(
            spread[0]["labelSelector"]["matchLabels"]["app.kubernetes.io/component"], "controller"
        )
        one = render(BASE)
        self.assertNotIn(
            "vmafx-controller", {d["metadata"]["name"] for d in of_kind(one, "PodDisruptionBudget")}
        )
        self.assertNotIn("topologySpreadConstraints", controller(one)["spec"]["template"]["spec"])

    def test_network_policy_opens_the_database_to_controller_and_migration(self) -> None:
        values = POSTGRES + "networkPolicy: {enabled: true}\n"
        docs = render(values, "--api-versions", CNPG_API)
        rule = named(docs, "NetworkPolicy", "vmafx-allow-controller-to-database")
        selector = rule["spec"]["podSelector"]["matchExpressions"][0]
        self.assertEqual(selector["key"], "app.kubernetes.io/component")
        self.assertEqual(sorted(selector["values"]), ["controller", "migrate"])
        egress = rule["spec"]["egress"][0]
        self.assertEqual(egress["ports"], [{"protocol": "TCP", "port": 5432}])
        self.assertEqual(
            egress["to"], [{"podSelector": {"matchLabels": {"cnpg.io/cluster": "vmafx-db"}}}]
        )
        sqlite = render(BASE + "networkPolicy: {enabled: true}\n")
        self.assertNotIn(
            "vmafx-allow-controller-to-database",
            {d["metadata"]["name"] for d in of_kind(sqlite, "NetworkPolicy")},
        )


if __name__ == "__main__":
    unittest.main()
