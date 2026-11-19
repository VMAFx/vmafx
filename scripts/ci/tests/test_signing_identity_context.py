#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Signing identities come from the repository and workflow that execute.

The publish workflows used to name ``VMAFx/vmafx`` and their own workflow path
in cosign certificate identities and in ``gh attestation verify -R``. A fork
running the same workflow then signed under one identity and verified another.
The executing context already carries both facts: ``GITHUB_WORKFLOW_REF`` is
``owner/repo/.github/workflows/file.yml@refs/...`` and ``GITHUB_REPOSITORY`` is
``owner/repo``.
"""

from __future__ import annotations

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
WORKFLOWS = {
    ".github/workflows/docker-publish-tester.yml": 2,
    ".github/workflows/docker-publish-operator-node.yml": 5,
    ".github/workflows/docker-publish-production.yml": 3,
}
IDENTITY = '"https://github.com/${GITHUB_WORKFLOW_REF}"'


class SigningIdentityContext(unittest.TestCase):
    def text(self, workflow: str) -> str:
        return (ROOT / workflow).read_text(encoding="utf-8")

    def test_no_workflow_hardcodes_the_upstream_identity(self) -> None:
        for workflow in WORKFLOWS:
            with self.subTest(workflow=workflow):
                text = self.text(workflow)
                self.assertNotIn("https://github.com/VMAFx/vmafx/.github/workflows/", text)
                self.assertNotIn("-R VMAFx/vmafx", text)

    def test_every_cosign_identity_is_the_executing_workflow_ref(self) -> None:
        for workflow, expected in WORKFLOWS.items():
            with self.subTest(workflow=workflow):
                text = self.text(workflow)
                self.assertEqual(text.count(IDENTITY), expected)

    def test_gh_attestation_uses_the_executing_repository(self) -> None:
        text = self.text(".github/workflows/docker-publish-tester.yml")
        self.assertIn('-R "${GITHUB_REPOSITORY}"', text)


if __name__ == "__main__":
    unittest.main()
