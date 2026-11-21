#!/usr/bin/env python3
# Copyright 2026 luoye-cpu
# SPDX-License-Identifier: EUPL-1.2
"""COSIGN_VERSION is the only owner of every cosign-installer input."""

from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location(
    "workflow_versions_cosign", ROOT / "scripts/ci/check-workflow-versions.py"
)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)

CHECK = MODULE.check_cosign_pins
WANT = "3.1.4"


class CosignVersionSingleSource(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.workflows = self.root / ".github/workflows"
        self.workflows.mkdir(parents=True)

    def write(self, text: str) -> Path:
        path = self.workflows / "sign.yml"
        path.write_text(text, encoding="utf-8")
        return path

    def fixture(self, env: str = '  COSIGN_VERSION: "3.1.4"\n', release: str = "") -> str:
        return (
            "name: sign\n"
            "env:\n"
            f"{env}"
            "jobs:\n"
            "  sign:\n"
            "    steps:\n"
            "      - uses: sigstore/cosign-installer\n"
            "        with:\n"
            f"          cosign-release: {release or 'v${{ env.COSIGN_VERSION }}'}\n"
        )

    def test_repository_uses_derived_inputs(self) -> None:
        want = MODULE.load_config(ROOT).get("COSIGN_VERSION")
        self.assertEqual(CHECK(ROOT, want), [])
        count = sum(
            len(MODULE.COSIGN_ANY_RE.findall(path.read_text(encoding="utf-8")))
            for path in sorted((ROOT / ".github/workflows").glob("*.yml"))
        )
        self.assertEqual(count, 25)

    def test_literal_input_fails(self) -> None:
        self.write(self.fixture(release='"v3.1.4"'))
        self.assertIn(
            "1 cosign-installer step(s) do not set cosign-release from env.COSIGN_VERSION",
            "\n".join(CHECK(self.root, WANT)),
        )

    def test_missing_env_fails(self) -> None:
        self.write(self.fixture(env=""))
        self.assertIn(
            "sign.yml must declare COSIGN_VERSION exactly once",
            "\n".join(CHECK(self.root, WANT)),
        )

    def test_duplicate_env_fails(self) -> None:
        self.write(self.fixture(env='  COSIGN_VERSION: "3.1.4"\n  COSIGN_VERSION: "3.1.4"\n'))
        self.assertIn(
            "sign.yml must declare COSIGN_VERSION exactly once",
            "\n".join(CHECK(self.root, WANT)),
        )

    def test_drifted_env_fails(self) -> None:
        self.write(self.fixture(env='  COSIGN_VERSION: "3.0.0"\n'))
        self.assertIn("pins COSIGN_VERSION '3.0.0'", "\n".join(CHECK(self.root, WANT)))

    def test_job_env_does_not_satisfy_the_mirror(self) -> None:
        self.write(
            "name: sign\n"
            "jobs:\n"
            "  sign:\n"
            "    env:\n"
            '      COSIGN_VERSION: "3.1.4"\n'
            "    steps:\n"
            "      - uses: sigstore/cosign-installer\n"
            "        with:\n"
            "          cosign-release: v${{ env.COSIGN_VERSION }}\n"
        )
        self.assertIn(
            "sign.yml must declare COSIGN_VERSION exactly once",
            "\n".join(CHECK(self.root, WANT)),
        )

    def test_suffix_after_the_expression_fails(self) -> None:
        self.write(self.fixture(release="v${{ env.COSIGN_VERSION }}-rc1"))
        self.assertIn(
            "1 cosign-installer step(s) do not set cosign-release from env.COSIGN_VERSION",
            "\n".join(CHECK(self.root, WANT)),
        )

    def test_installer_step_without_the_input_fails(self) -> None:
        self.write(
            "name: sign\n"
            "env:\n"
            '  COSIGN_VERSION: "3.1.4"\n'
            "jobs:\n"
            "  sign:\n"
            "    steps:\n"
            "      - uses: sigstore/cosign-installer\n"
        )
        self.assertIn(
            "1 cosign-installer step(s) do not set cosign-release from env.COSIGN_VERSION",
            "\n".join(CHECK(self.root, WANT)),
        )

    def test_renovate_manager_owns_config_and_workflows(self) -> None:
        data = json.loads((ROOT / "renovate.json").read_text(encoding="utf-8"))
        managers = [
            item
            for item in data["customManagers"]
            if item.get("depNameTemplate") == "sigstore/cosign"
        ]
        self.assertEqual(len(managers), 1)
        manager = managers[0]
        self.assertEqual(manager["datasourceTemplate"], "github-releases")
        patterns = " ".join(manager["managerFilePatterns"])
        self.assertIn("build-config", patterns)
        self.assertIn(".github/workflows", patterns)
        self.assertTrue(all("COSIGN_VERSION" in item for item in manager["matchStrings"]))


if __name__ == "__main__":
    unittest.main()
