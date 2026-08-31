#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Guard the Kubernetes E2E workflow's executable runtime contract."""

from __future__ import annotations

import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "e2e-k8s.yml"
RULES_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "rule-enforcement.yml"
NODE_DOCKERFILE = REPO_ROOT / "docker" / "Dockerfile.node"
SERVER_DOCKERFILE = REPO_ROOT / "Dockerfile.go-server"
GITIGNORE = REPO_ROOT / ".gitignore"
KIND_SCRIPT = REPO_ROOT / "test" / "e2e" / "kind-cluster.sh"
ASSERT_CONTEXT_SCRIPT = REPO_ROOT / "test" / "e2e" / "assert-kind-context.sh"
KUTTL_CASES = REPO_ROOT / "test" / "e2e" / "kuttl-tests"
KUTTL_CONFIG = KUTTL_CASES / "kuttl-test.yaml"
INSTALL_STEP = KUTTL_CASES / "01-chart-cpu-score" / "00-install.yaml"
READY_STEP = KUTTL_CASES / "01-chart-cpu-score" / "01-ready.yaml"
SCORE_STEP = KUTTL_CASES / "01-chart-cpu-score" / "02-score.yaml"
SCORE_SCRIPT = REPO_ROOT / "test" / "e2e" / "score-smoke.sh"
CHART_TEMPLATES = REPO_ROOT / "deploy" / "helm" / "vmafx" / "templates"
NODE_BUILD_STEP = "Build vmafx-node image (cpu variant, e2e tag)"
SERVER_BUILD_STEP = "Build vmafx-server image (cpu variant, e2e tag)"
IMAGE_NAMES = (
    "ghcr.io/vmafx/vmafx-operator:e2e-test",
    "ghcr.io/vmafx/vmafx-node:e2e-test",
    "ghcr.io/vmafx/vmafx-server:e2e-test",
)


def _workflow_step(workflow: str, name: str) -> str:
    """Return one top-level step block from the E2E workflow."""
    marker = f"      - name: {name}\n"
    start = workflow.find(marker)
    if start < 0:
        raise AssertionError(f"workflow step not found: {name}")

    end = workflow.find("\n      - name:", start + len(marker))
    if end < 0:
        end = len(workflow)
    return workflow[start:end]


class E2ERuntimeContractTest(unittest.TestCase):
    """Keep the nightly test aligned with artifacts it actually executes."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.workflow = WORKFLOW.read_text(encoding="utf-8")

    def test_cpu_node_and_server_targets_are_explicit(self) -> None:
        node_step = _workflow_step(self.workflow, NODE_BUILD_STEP)
        server_step = _workflow_step(self.workflow, SERVER_BUILD_STEP)

        self.assertRegex(node_step, r"(?m)^\s+file:\s+docker/Dockerfile\.node\s*$")
        self.assertRegex(node_step, r"(?m)^\s+target:\s+node-cpu\s*$")
        self.assertNotIn("BACKEND=cpu", node_step)
        self.assertRegex(server_step, r"(?m)^\s+file:\s+Dockerfile\.go-server\s*$")
        self.assertRegex(server_step, r"(?m)^\s+target:\s+go-server\s*$")

        node_dockerfile = NODE_DOCKERFILE.read_text(encoding="utf-8")
        server_dockerfile = SERVER_DOCKERFILE.read_text(encoding="utf-8")
        self.assertRegex(node_dockerfile, r"(?m)^FROM\s+runtime-base\s+AS\s+node-cpu\s*$")
        self.assertIn("cp -r model/. /dist/model/", node_dockerfile)
        self.assertIn("test -f /dist/model/vmaf_v0.6.1.json", node_dockerfile)
        self.assertNotIn("cp -r model/ /dist/model/", node_dockerfile)
        self.assertRegex(server_dockerfile, r"(?m)^FROM\s+.+\s+AS\s+go-server\s*$")

    def test_all_runtime_images_are_exported_and_loaded(self) -> None:
        export_step = _workflow_step(self.workflow, "Export images as tar for transfer to e2e job")
        load_step = _workflow_step(self.workflow, "Load e2e images into kind cluster")

        for image in IMAGE_NAMES:
            with self.subTest(image=image):
                self.assertIn(image, export_step)
                self.assertIn(image, load_step)

    def test_chart_smoke_uses_exact_local_images_and_cpu(self) -> None:
        install = INSTALL_STEP.read_text(encoding="utf-8")
        ready = READY_STEP.read_text(encoding="utf-8")
        score = SCORE_STEP.read_text(encoding="utf-8")

        self.assertIn("helm upgrade --install vmafx", install)
        self.assertIn("--set image.tag=e2e-test", install)
        self.assertIn("--set image.pullPolicy=Never", install)
        self.assertIn("--set operator.image.tag=e2e-test", install)
        self.assertIn("--set operator.image.pullPolicy=Never", install)
        self.assertIn("--set gpu.vendor=cpu", install)
        self.assertIn("vmafx-e2e-fixtures", install)
        self.assertIn("--type=strategic", install)
        self.assertIn("kubectl wait --for=condition=Established", ready)
        self.assertIn("kubectl wait --for=condition=Available", ready)
        self.assertFalse((READY_STEP.parent / "01-assert.yaml").exists())
        self.assertIn("bash ../../score-smoke.sh", score)

        score_script = SCORE_SCRIPT.read_text(encoding="utf-8")
        self.assertIn("isinstance(score, bool)", score_script)
        self.assertIn("isinstance(feature_vmaf, bool)", score_script)

    def test_chart_service_selects_only_server_workloads(self) -> None:
        service = (CHART_TEMPLATES / "service.yaml").read_text(encoding="utf-8")
        deployment = (CHART_TEMPLATES / "deployment.yaml").read_text(encoding="utf-8")
        statefulset = (CHART_TEMPLATES / "statefulset.yaml").read_text(encoding="utf-8")
        job = (CHART_TEMPLATES / "job.yaml").read_text(encoding="utf-8")

        self.assertGreaterEqual(service.count("app.kubernetes.io/component: server"), 2)
        self.assertGreaterEqual(deployment.count("app.kubernetes.io/component: server"), 2)
        self.assertGreaterEqual(statefulset.count("app.kubernetes.io/component: server"), 4)
        self.assertGreaterEqual(job.count("app.kubernetes.io/component: server"), 2)

    def test_impossible_legacy_scenarios_are_absent(self) -> None:
        legacy_cases = (
            "02-vmafxjob-creates-pod",
            "03-node-heartbeat",
            "04-rclone-score",
            "05-sidecar-trainer",
        )

        for case in legacy_cases:
            with self.subTest(case=case):
                self.assertFalse((KUTTL_CASES / case).exists())

        kind_script = KIND_SCRIPT.read_text(encoding="utf-8")
        self.assertNotIn("helm upgrade --install vmafx-crds", kind_script)
        self.assertNotIn("cert-manager/cert-manager/releases", kind_script)
        self.assertNotIn("fake-device-plugin", kind_script)
        self.assertIn(
            'kubectl --kubeconfig "${KUBECONFIG_PATH}" apply --server-side',
            kind_script,
        )
        self.assertIn('-f "${REPO_ROOT}/deploy/helm/vmafx/crds/"', kind_script)

    def test_contract_runs_in_always_on_rules_workflow(self) -> None:
        rules = RULES_WORKFLOW.read_text(encoding="utf-8")
        invocation = "python3 scripts/ci/test_e2e_runtime_contract.py"

        self.assertIn(invocation, rules)
        self.assertIn(invocation, self.workflow)

    def test_tools_are_runner_local_and_results_need_no_write_token(self) -> None:
        for tool in ("kind", "kubectl", "kuttl"):
            with self.subTest(tool=tool):
                name = f"Install {tool} ${{{{ env.{tool.upper()}_VERSION }}}}"
                step = _workflow_step(self.workflow, name)
                self.assertIn("${RUNNER_TEMP}/vmafx-e2e-tools/bin", step)
                self.assertIn("--retry 5 --retry-all-errors", step)
                self.assertIn('>> "${GITHUB_PATH}"', step)
                self.assertNotIn("/usr/local/bin", step)

        publish = _workflow_step(self.workflow, "Publish test results")
        self.assertIn("hashFiles('test/e2e/results/**/*.xml') != ''", publish)
        self.assertRegex(publish, r"(?m)^\s+check_run:\s+false\s*$")

    def test_kubernetes_mutations_use_an_isolated_kind_context(self) -> None:
        kind_script = KIND_SCRIPT.read_text(encoding="utf-8")
        context_guard = ASSERT_CONTEXT_SCRIPT.read_text(encoding="utf-8")
        kuttl_config = KUTTL_CONFIG.read_text(encoding="utf-8")
        gitignore = GITIGNORE.read_text(encoding="utf-8")

        self.assertIn('kubeconfig_dir="${RUNNER_TEMP}/vmafx-e2e"', self.workflow)
        self.assertIn("printf 'VMAFX_E2E_KUBECONFIG=%s", self.workflow)
        self.assertIn('>> "${GITHUB_ENV}"', self.workflow)
        self.assertNotIn("${{ runner.temp }}", self.workflow)
        self.assertGreaterEqual(self.workflow.count("bash test/e2e/assert-kind-context.sh"), 2)
        self.assertIn("--config test/e2e/kuttl-tests/kuttl-test.yaml", self.workflow)
        self.assertNotIn("kind-cluster.sh --teardown || true", self.workflow)
        self.assertIn('KUBECONFIG_PATH="${VMAFX_E2E_KUBECONFIG:-}"', kind_script)
        self.assertIn('kind create cluster --name "${CLUSTER_NAME}"', kind_script)
        self.assertGreaterEqual(kind_script.count('"${ASSERT_CONTEXT}"'), 3)
        self.assertIn('kubectl --kubeconfig "${KUBECONFIG_PATH}" apply', kind_script)
        self.assertIn('kind delete cluster --name "${CLUSTER_NAME}"', kind_script)
        self.assertIn('EXPECTED_CONTEXT="kind-${CLUSTER_NAME}"', context_guard)
        self.assertIn(r"^https://127\.0\.0\.1:[0-9]+$", context_guard)
        self.assertIn("the process-wide default kubeconfig", context_guard)
        self.assertIn("KUBECONFIG must equal VMAFX_E2E_KUBECONFIG", context_guard)
        self.assertRegex(gitignore, r"(?m)^/kubeconfig$")
        self.assertRegex(kuttl_config, r"(?m)^skipDelete:\s+true\s*$")


if __name__ == "__main__":
    unittest.main()
