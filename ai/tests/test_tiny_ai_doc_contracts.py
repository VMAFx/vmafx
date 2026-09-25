# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
#
# ai/tests/test_tiny_ai_doc_contracts.py — executable contract tests for the
# three docs/ai runtime-contract gaps closed in issue #1242:
#   1. Sidecar quarantine in docs/ai/sidecar-online-training.md (Research-0733 §3.4)
#   2. Shipped transnet_v2.c sliding-window contract in docs/ai/extractor-template.md
#   3. Self-hosted inference runner and cross-device parity in docs/ai/inference.md
#
# Follows ADR-0100, ADR-0042, ADR-0781, ADR-1319, and Research-2029.

from __future__ import annotations

import pathlib
import re
import unittest
import urllib.parse

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]


class SidecarQuarantineDocContractTest(unittest.TestCase):
    """Pin the sidecar quarantine doc contract and code truth (Research-0733 §3.4)."""

    def setUp(self) -> None:
        self.doc_path = REPO_ROOT / "docs" / "ai" / "sidecar-online-training.md"
        self.assertTrue(self.doc_path.is_file(), f"Missing doc at {self.doc_path}")
        self.doc_text = self.doc_path.read_text(encoding="utf-8")

    def test_quarantine_section_exists_and_unambiguous(self) -> None:
        self.assertIn("## Checkpoint quarantine (not implemented)", self.doc_text)
        self.assertIn("Research-0733 §3.4", self.doc_text)
        self.assertTrue(re.search(r"None of\s+it exists in the tree", self.doc_text))

    def test_research_0733_section_3_4_elements_accounted_for(self) -> None:
        # Every §3.4 element must be explicitly categorized as implemented or not
        required_elements = (
            "Atomic checkpoint write",
            "model.onnx.sha256",
            "verified by the node before load",
            "status.modelVersion",
            "spec.versionPolicy",
            "Fixture set for the stability gate",
            "PLCC comparison job in the controller",
            "unstable",
            "stability_plcc_delta",
            "rollback_threshold",
        )
        for element in required_elements:
            with self.subTest(element=element):
                self.assertIn(element, self.doc_text)

    def test_python_sidecar_atomic_export_and_sha256_truth(self) -> None:
        sgd_ema_py = (REPO_ROOT / "ai" / "sidecar" / "sgd_ema.py").read_text(encoding="utf-8")
        online_trainer_py = (REPO_ROOT / "ai" / "sidecar" / "online_trainer.py").read_text(
            encoding="utf-8"
        )
        # Atomic export implementation
        self.assertIn("os.replace", sgd_ema_py)
        self.assertIn(".tmp.onnx", sgd_ema_py)
        # sha256 sidecar implementation
        self.assertIn("_write_sha256_sidecar", online_trainer_py)
        self.assertIn(".sha256", online_trainer_py)

    def test_node_and_crd_quarantine_absence_truth(self) -> None:
        # Verify node has zero sha256 checking logic for checkpoints
        node_dir = REPO_ROOT / "cmd" / "vmafx-node"
        if node_dir.is_dir():
            for go_file in node_dir.rglob("*.go"):
                content = go_file.read_text(encoding="utf-8")
                self.assertNotIn("sha256.Sum256", content)
                self.assertNotIn(".sha256", content)

        # Verify CRD does not have spec.versionPolicy
        crd_path = (
            REPO_ROOT / "deploy" / "helm" / "vmafx" / "crds" / "vmafx.dev_vmafxmodeltrainings.yaml"
        )
        if crd_path.is_file():
            crd_text = crd_path.read_text(encoding="utf-8")
            self.assertNotIn("versionPolicy", crd_text)

        # Verify Go types lack VersionPolicy in spec
        types_path = REPO_ROOT / "api" / "vmafx" / "v1" / "vmafxmodeltraining_types.go"
        if types_path.is_file():
            types_text = types_path.read_text(encoding="utf-8")
            self.assertNotIn("VersionPolicy", types_text)
            self.assertIn("ModelVersion", types_text)


class ExtractorTemplateDocContractTest(unittest.TestCase):
    """Pin the TransNet V2 sliding-window contract in extractor-template.md."""

    def setUp(self) -> None:
        self.doc_path = REPO_ROOT / "docs" / "ai" / "extractor-template.md"
        self.assertTrue(self.doc_path.is_file(), f"Missing doc at {self.doc_path}")
        self.doc_text = self.doc_path.read_text(encoding="utf-8")

    def test_shipped_transnet_v2_path_and_name_documented(self) -> None:
        self.assertIn("core/src/feature/transnet_v2.c", self.doc_text)
        self.assertIn("Large sliding windows (`transnet_v2`)", self.doc_text)
        # Confirms distinction between former 'feature_transnet_v2.c' and shipped path
        self.assertIn("not `feature_transnet_v2.c`", self.doc_text)

    def test_transnet_v2_code_registration_truth(self) -> None:
        c_source = REPO_ROOT / "core" / "src" / "feature" / "transnet_v2.c"
        self.assertTrue(c_source.is_file(), f"Missing C source at {c_source}")

        meson_build = (REPO_ROOT / "core" / "src" / "meson.build").read_text(encoding="utf-8")
        self.assertIn("transnet_v2.c", meson_build)

        fex_cpp = (REPO_ROOT / "core" / "src" / "feature" / "feature_extractor.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn("vmaf_fex_transnet_v2", fex_cpp)

    def test_transnet_v2_sliding_window_contract_truth(self) -> None:
        c_text = (REPO_ROOT / "core" / "src" / "feature" / "transnet_v2.c").read_text(
            encoding="utf-8"
        )
        # Tensor contracts
        self.assertIn("TRANSNET_V2_WINDOW 100u", c_text)
        self.assertIn("TRANSNET_V2_HEIGHT 27u", c_text)
        self.assertIn("TRANSNET_V2_WIDTH 48u", c_text)
        self.assertIn("boundary_logits", c_text)
        self.assertIn("shot_boundary_probability", c_text)
        self.assertIn("shot_boundary", c_text)

        # Doc specifies [1, 100, 3, 27, 48] input and [1, 100] output
        self.assertIn("[1, 100, 3, 27, 48]", self.doc_text)
        self.assertIn("[1, 100]", self.doc_text)
        self.assertIn("No decimation", self.doc_text)
        self.assertIn("Warm-up, not padding", self.doc_text)
        self.assertIn("Per-shot aggregation is not shipped", self.doc_text)
        self.assertIn("T6-3b", self.doc_text)


class InferenceRunnerDocContractTest(unittest.TestCase):
    """Pin the self-hosted inference runner and cross-device parity doc contract."""

    def setUp(self) -> None:
        self.doc_path = REPO_ROOT / "docs" / "ai" / "inference.md"
        self.assertTrue(self.doc_path.is_file(), f"Missing doc at {self.doc_path}")
        self.doc_text = self.doc_path.read_text(encoding="utf-8")

    def test_cross_device_parity_documented_as_ungated(self) -> None:
        self.assertIn("No CI job checks tiny-AI cross-device parity today", self.doc_text)
        self.assertIn("workstation measurements, not gated numbers", self.doc_text)
        self.assertIn("1e-4", self.doc_text)
        self.assertIn("1e-2", self.doc_text)

    def test_workflow_identities_and_labels_match_truth(self) -> None:
        self.assertIn("sycl-parity.yml", self.doc_text)
        self.assertIn("SYCL_ARC_RUNNER_ENABLED", self.doc_text)
        self.assertIn("sycl-arc", self.doc_text)
        self.assertIn("tests-and-quality-gates.yml", self.doc_text)
        self.assertIn("GPU_COVERAGE_ENABLED", self.doc_text)
        self.assertIn("gpu-full", self.doc_text)

        # Verify workflows exist and declare these exact variables/labels
        tests_wf = (REPO_ROOT / ".github" / "workflows" / "tests-and-quality-gates.yml").read_text(
            encoding="utf-8"
        )
        self.assertIn("GPU_COVERAGE_ENABLED", tests_wf)
        self.assertIn("gpu-full", tests_wf)

        sycl_wf = (REPO_ROOT / ".github" / "workflows" / "sycl-parity.yml").read_text(
            encoding="utf-8"
        )
        self.assertIn("SYCL_ARC_RUNNER_ENABLED", sycl_wf)
        self.assertIn("sycl-arc", sycl_wf)

    def test_state_ledger_tracks_open_ungated_parity_row(self) -> None:
        state_text = (REPO_ROOT / "docs" / "state.md").read_text(encoding="utf-8")
        self.assertIn("T-TINY-AI-CROSS-DEVICE-PARITY-UNGATED-2026-09-25", state_text)


class MarkdownLinksResolutionTest(unittest.TestCase):
    """Verify that all relative file links in the three docs resolve to real files."""

    DOC_FILES = (
        "docs/ai/sidecar-online-training.md",
        "docs/ai/extractor-template.md",
        "docs/ai/inference.md",
    )

    def test_relative_links_resolve_on_disk(self) -> None:
        for rel_doc in self.DOC_FILES:
            doc_path = REPO_ROOT / rel_doc
            doc_text = doc_path.read_text(encoding="utf-8")

            # Extract markdown links: [text](target)
            links = re.findall(r"\[([^\]]+)\]\(([^)]+)\)", doc_text)
            for text, target in links:
                target = target.strip()
                # Skip external web URLs
                if target.startswith(("http://", "https://", "mailto:")):
                    continue
                # Same-page anchors (#anchor)
                if target.startswith("#"):
                    continue

                # Strip anchor fragment from target path if present
                clean_target = urllib.parse.unquote(target.split("#", 1)[0])
                resolved = (doc_path.parent / clean_target).resolve()

                with self.subTest(doc=rel_doc, text=text, target=target):
                    self.assertTrue(
                        resolved.exists(),
                        f"In {rel_doc}, link [{text}]({target}) resolves to missing path: {resolved}",
                    )


if __name__ == "__main__":
    unittest.main()
