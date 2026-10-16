#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Unit tests for ONNX Runtime tarball SHA-256 pin verification (ADR-3334)."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "scripts/ci/check_ort_tarball_pins.py"


class OrtTarballSingleSourceTest(unittest.TestCase):
    def run_check(
        self, *args: str | Path, cwd: Path | None = None
    ) -> subprocess.CompletedProcess[str]:
        cmd = [sys.executable, str(SCRIPT)] + [str(a) for a in args]
        return subprocess.run(  # noqa: S603
            cmd,
            cwd=cwd or ROOT,
            capture_output=True,
            text=True,
            check=False,
        )

    def test_repo_tree_passes(self) -> None:
        """Case 1: Live repository tree passes all ORT tarball checks."""
        proc = self.run_check()
        self.assertEqual(
            proc.returncode,
            0,
            f"Expected exit 0 for repo tree, got {proc.returncode}.\nStdout:\n{proc.stdout}\nStderr:\n{proc.stderr}",
        )
        self.assertEqual(proc.stdout.strip(), "")

    def test_planted_site_without_sha256(self) -> None:
        """Case 2: Planted site without ORT_SHA256 fails with exit 1."""
        with tempfile.TemporaryDirectory() as tmpdir:
            wf = Path(tmpdir) / ".github/workflows/test.yml"
            wf.parent.mkdir(parents=True, exist_ok=True)
            wf.write_text(
                textwrap.dedent("""\
                    jobs:
                      build:
                        steps:
                          - name: Install ORT
                            env:
                              ORT_VERSION: "1.30.0"
                            run: |
                              set -euo pipefail
                              curl -fsSL "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-linux-x64-${ORT_VERSION}.tgz" -o /tmp/ort.tgz
                              echo "a5ed5a3cac51fbb2e90da632ae43d19212faaa20e76484e62bcb7c23ddb3b3fd  /tmp/ort.tgz" | sha256sum --check --strict -
                              sudo tar -xzf /tmp/ort.tgz -C /opt/
                    """),
                encoding="utf-8",
            )
            proc = self.run_check(wf)
            self.assertEqual(proc.returncode, 1)
            self.assertIn("missing ORT_SHA256", proc.stdout)

    def test_planted_tar_before_check(self) -> None:
        """Case 3: Planted tar -x before sha256sum --check fails with exit 1."""
        with tempfile.TemporaryDirectory() as tmpdir:
            wf = Path(tmpdir) / ".github/workflows/test.yml"
            wf.parent.mkdir(parents=True, exist_ok=True)
            wf.write_text(
                textwrap.dedent("""\
                    jobs:
                      build:
                        steps:
                          - name: Install ORT
                            env:
                              ORT_VERSION: "1.30.0"
                              ORT_SHA256: "a5ed5a3cac51fbb2e90da632ae43d19212faaa20e76484e62bcb7c23ddb3b3fd"
                            run: |
                              set -euo pipefail
                              curl -fsSL "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-linux-x64-${ORT_VERSION}.tgz" -o /tmp/ort.tgz
                              sudo tar -xzf /tmp/ort.tgz -C /opt/
                              printf '%s  %s\\n' "${ORT_SHA256}" /tmp/ort.tgz | sha256sum --check --strict
                    """),
                encoding="utf-8",
            )
            proc = self.run_check(wf)
            self.assertEqual(proc.returncode, 1)
            self.assertIn("tar extraction occurs before sha256sum --check", proc.stdout)

    def test_planted_two_sites_same_version_different_sha(self) -> None:
        """Case 4: Two sites with same flavour and version but different sha fail with exit 1."""
        with tempfile.TemporaryDirectory() as tmpdir:
            wf1 = Path(tmpdir) / ".github/workflows/wf1.yml"
            wf2 = Path(tmpdir) / ".github/workflows/wf2.yml"
            wf1.parent.mkdir(parents=True, exist_ok=True)
            wf1.write_text(
                textwrap.dedent("""\
                    jobs:
                      build:
                        steps:
                          - name: Install ORT
                            env:
                              ORT_VERSION: "1.30.0"
                              ORT_SHA256: "a5ed5a3cac51fbb2e90da632ae43d19212faaa20e76484e62bcb7c23ddb3b3fd"
                            run: |
                              set -euo pipefail
                              curl -fsSL "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-linux-x64-${ORT_VERSION}.tgz" -o /tmp/ort.tgz
                              printf '%s  %s\\n' "${ORT_SHA256}" /tmp/ort.tgz | sha256sum --check --strict
                              sudo tar -xzf /tmp/ort.tgz -C /opt/
                    """),
                encoding="utf-8",
            )
            wf2.write_text(
                textwrap.dedent("""\
                    jobs:
                      build:
                        steps:
                          - name: Install ORT
                            env:
                              ORT_VERSION: "1.30.0"
                              ORT_SHA256: "1111111111111111111111111111111111111111111111111111111111111111"
                            run: |
                              set -euo pipefail
                              curl -fsSL "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-linux-x64-${ORT_VERSION}.tgz" -o /tmp/ort.tgz
                              printf '%s  %s\\n' "${ORT_SHA256}" /tmp/ort.tgz | sha256sum --check --strict
                              sudo tar -xzf /tmp/ort.tgz -C /opt/
                    """),
                encoding="utf-8",
            )
            proc = self.run_check(wf1, wf2)
            self.assertEqual(proc.returncode, 1)
            self.assertIn("conflicting sha256 for (cpu, 1.30.0)", proc.stdout)

    def test_planted_malformed_hex(self) -> None:
        """Case 5: Planted malformed hex digest in ORT_SHA256 fails with exit 1."""
        with tempfile.TemporaryDirectory() as tmpdir:
            wf = Path(tmpdir) / ".github/workflows/test.yml"
            wf.parent.mkdir(parents=True, exist_ok=True)
            wf.write_text(
                textwrap.dedent("""\
                    jobs:
                      build:
                        steps:
                          - name: Install ORT
                            env:
                              ORT_VERSION: "1.30.0"
                              ORT_SHA256: "not-a-valid-sha256-hex"
                            run: |
                              set -euo pipefail
                              curl -fsSL "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-linux-x64-${ORT_VERSION}.tgz" -o /tmp/ort.tgz
                              printf '%s  %s\\n' "${ORT_SHA256}" /tmp/ort.tgz | sha256sum --check --strict
                              sudo tar -xzf /tmp/ort.tgz -C /opt/
                    """),
                encoding="utf-8",
            )
            proc = self.run_check(wf)
            self.assertEqual(proc.returncode, 1)
            self.assertIn("malformed ORT_SHA256", proc.stdout)

    def test_unreadable_path(self) -> None:
        """Case 6: Unreadable or missing path fails with exit 2."""
        proc = self.run_check(Path("/nonexistent/file/path/here.yml"))
        self.assertEqual(proc.returncode, 2)
        self.assertIn("check_ort_tarball_pins: error:", proc.stderr)

    def test_boundary_gpu_cpu_same_version_different_sha(self) -> None:
        """Case 7: Boundary: GPU and CPU of same version with different sha pass with exit 0."""
        with tempfile.TemporaryDirectory() as tmpdir:
            wf_cpu = Path(tmpdir) / ".github/workflows/cpu.yml"
            wf_gpu = Path(tmpdir) / ".github/workflows/gpu.yml"
            wf_cpu.parent.mkdir(parents=True, exist_ok=True)
            wf_cpu.write_text(
                textwrap.dedent("""\
                    jobs:
                      build:
                        steps:
                          - name: Install ORT CPU
                            env:
                              ORT_VERSION: "1.22.0"
                              ORT_SHA256: "8344d55f93d5bc5021ce342db50f62079daf39aaafb5d311a451846228be49b3"
                            run: |
                              set -euo pipefail
                              curl -fsSL "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-linux-x64-${ORT_VERSION}.tgz" -o /tmp/ort.tgz
                              printf '%s  %s\\n' "${ORT_SHA256}" /tmp/ort.tgz | sha256sum --check --strict
                              sudo tar -xzf /tmp/ort.tgz -C /opt/
                    """),
                encoding="utf-8",
            )
            wf_gpu.write_text(
                textwrap.dedent("""\
                    jobs:
                      build:
                        steps:
                          - name: Install ORT GPU
                            env:
                              ORT_VERSION: "1.22.0"
                              ORT_SHA256: "2a19dbfa403672ec27378c3d40a68f793ac7a6327712cd0e8240a86be2b10c55"
                            run: |
                              set -euo pipefail
                              curl -fsSL "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-linux-x64-gpu-${ORT_VERSION}.tgz" -o /tmp/ort-gpu.tgz
                              printf '%s  %s\\n' "${ORT_SHA256}" /tmp/ort-gpu.tgz | sha256sum --check --strict
                              sudo tar -xzf /tmp/ort-gpu.tgz -C /opt/
                    """),
                encoding="utf-8",
            )
            proc = self.run_check(wf_cpu, wf_gpu)
            self.assertEqual(
                proc.returncode,
                0,
                f"Expected exit 0 for CPU+GPU of same version, got {proc.returncode}.\nStdout:\n{proc.stdout}\nStderr:\n{proc.stderr}",
            )


if __name__ == "__main__":
    unittest.main()
