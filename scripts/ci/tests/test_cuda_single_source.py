#!/usr/bin/env python3
# Copyright 2026 Lusoris
# Copyright 2026 Claude (Anthropic)
# SPDX-License-Identifier: EUPL-1.2
"""Verify every current CUDA installer consumes build-config.env."""

from __future__ import annotations

import importlib.util
import shutil
import tempfile
import unittest
from pathlib import Path
from typing import Protocol, cast

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location(
    "workflow_versions", ROOT / "scripts/ci/check-workflow-versions.py"
)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class WorkflowGate(Protocol):
    CUDA_LINUX_INSTALLER: str
    CUDA_WINDOWS_INSTALLER: str

    def check_cuda(self, root: Path, config: dict[str, str]) -> list[str]: ...

    def load_config(self, root: Path) -> dict[str, str]: ...


GATE = cast(WorkflowGate, MODULE)


class CudaSingleSource(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.repo = Path(self.temporary.name)
        for path in (
            "build-config.env",
            "dev/Containerfile",
            ".github/workflows/build.yml",
            ".github/workflows/libvmaf-build-matrix.yml",
            GATE.CUDA_LINUX_INSTALLER,
            GATE.CUDA_WINDOWS_INSTALLER,
        ):
            source = ROOT / path
            target = self.repo / path
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)

    def check(self) -> list[str]:
        return GATE.check_cuda(self.repo, GATE.load_config(self.repo))

    def test_existing_consumers_pass(self) -> None:
        self.assertEqual(self.check(), [])

    def test_apt_package_must_match_configured_series(self) -> None:
        config = self.repo / "build-config.env"
        config.write_text(config.read_text().replace("cuda-toolkit-13-4", "cuda-toolkit-13-3"))
        self.assertTrue(self.check())

    def test_container_must_source_config_before_install(self) -> None:
        container = self.repo / "dev/Containerfile"
        text = container.read_text()
        for changed in (
            text.replace(". /opt/vmafx/build-config.env", "true", 1),
            text.replace('"${CUDA_APT_PACKAGE}"', "cuda-toolkit-13-4", 1),
            text.replace("COPY build-config.env /opt/vmafx/build-config.env", "", 1),
        ):
            with self.subTest(change=changed[:80]):
                container.write_text(changed)
                self.assertTrue(self.check())
            container.write_text(text)

    def test_workflows_use_shared_installers_only(self) -> None:
        workflow = self.repo / ".github/workflows/build.yml"
        text = workflow.read_text()
        workflow.write_text(
            text.replace(
                "run: scripts/ci/install-cuda-linux.sh",
                "uses: Jimver/cuda-toolkit@deadbeef\n        with:\n          cuda: '13.4.1'",
                1,
            )
        )
        problems = self.check()
        self.assertTrue(any("Jimver" in problem for problem in problems))
        self.assertTrue(any("CUDA_VERSION" in problem for problem in problems))

    def test_windows_helper_pins_architecture_and_verifies_download(self) -> None:
        text = (self.repo / GATE.CUDA_WINDOWS_INSTALLER).read_text()
        self.assertIn("windows_${Architecture}_network.exe", text)
        self.assertIn("Get-FileHash", text)
        self.assertIn("Get-AuthenticodeSignature", text)
        self.assertIn("SignatureStatus]::Valid", text)
        self.assertIn("SignerCertificate.Subject", text)
        self.assertIn('notmatch "(?i)NVIDIA"', text)
        self.assertIn("CUDA_VERSION", text)

    def test_linux_helper_uses_supported_native_repositories(self) -> None:
        text = (self.repo / GATE.CUDA_LINUX_INSTALLER).read_text()
        self.assertIn("ubuntu:24.04 | ubuntu:26.04", text)
        self.assertIn('"cuda-nvcc-${cuda_series}"', text)
        self.assertIn('"cuda-cudart-dev-${cuda_series}"', text)


if __name__ == "__main__":
    unittest.main()
