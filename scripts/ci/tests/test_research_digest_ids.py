#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Red-cap contract tests for the research-digest identifier ratchet."""

from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path
from types import ModuleType

ROOT = Path(__file__).resolve().parents[3]
CHECKER_PATH = ROOT / "scripts/ci/check-research-digest-ids.py"


def _load_checker() -> ModuleType:
    spec = importlib.util.spec_from_file_location("check_research_digest_ids", CHECKER_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot import {CHECKER_PATH}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


CHECKER = _load_checker()


class ResearchDigestIdTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tempdir = tempfile.TemporaryDirectory()
        self.addCleanup(self.tempdir.cleanup)
        self.root = Path(self.tempdir.name)
        (self.root / "docs/research").mkdir(parents=True)
        self.baseline = self.root / "scripts/ci/research-digest-id-baseline.json"

    def _digest(self, name: str, heading: str | None = None) -> Path:
        path = self.root / "docs/research" / name
        number = name[:4]
        path.write_text(
            f"{heading or f'# Research-{number}: fixture'}\n\nFixture.\n",
            encoding="utf-8",
        )
        return path

    def _write_baseline(self) -> None:
        CHECKER.write_baseline(self.root, self.baseline)

    def test_clean_unique_digests_pass(self) -> None:
        self._digest("1000-alpha.md")
        self._digest("1001-beta.md")
        self._write_baseline()
        self.assertEqual(CHECKER.audit_repository(self.root, self.baseline), [])

    def test_new_collision_fails_closed(self) -> None:
        self._digest("1000-alpha.md")
        self._write_baseline()
        self._digest("1000-beta.md")

        errors = CHECKER.audit_repository(self.root, self.baseline)
        self.assertEqual(len(errors), 1)
        self.assertIn("Research-1000 collision set drift", errors[0])
        self.assertIn("1000-beta.md", errors[0])

    def test_removed_or_renamed_baseline_member_fails_closed(self) -> None:
        first = self._digest("1000-alpha.md")
        second = self._digest("1000-beta.md")
        self._write_baseline()

        second.unlink()
        errors = CHECKER.audit_repository(self.root, self.baseline)
        self.assertIn("Research-1000 collision set drift", errors[0])

        second = self._digest("1000-beta.md")
        second.rename(second.with_name("1000-gamma.md"))
        errors = CHECKER.audit_repository(self.root, self.baseline)
        self.assertIn("Research-1000 collision set drift", errors[0])
        self.assertIn("1000-gamma.md", errors[0])
        self.assertTrue(first.exists())

    def test_h1_filename_mismatch_fails_closed(self) -> None:
        path = self._digest("1000-alpha.md")
        self._write_baseline()
        path.write_text("# Research-1001: wrong ID\n", encoding="utf-8")

        errors = CHECKER.audit_repository(self.root, self.baseline)
        self.assertEqual(len(errors), 1)
        self.assertIn("docs/research/1000-alpha.md H1 drift", errors[0])
        self.assertIn("canonical form", errors[0])

    def test_baseline_write_is_deterministic(self) -> None:
        self._digest("1000-zeta.md", "# Legacy heading")
        self._digest("1000-alpha.md")
        self._write_baseline()
        first = self.baseline.read_bytes()
        self._write_baseline()

        self.assertEqual(self.baseline.read_bytes(), first)
        payload = json.loads(first)
        self.assertEqual(
            payload["legacy_collisions"]["1000"],
            [
                "docs/research/1000-alpha.md",
                "docs/research/1000-zeta.md",
            ],
        )
        self.assertEqual(
            payload["legacy_heading_exceptions"],
            {"docs/research/1000-zeta.md": "# Legacy heading"},
        )

    def test_write_refuses_to_absorb_new_debt(self) -> None:
        path = self._digest("1000-alpha.md")
        self._write_baseline()

        self._digest("1000-beta.md")
        with self.assertRaisesRegex(CHECKER.GateError, "adds collision members"):
            self._write_baseline()

        path.write_text("# Research-1001: wrong ID\n", encoding="utf-8")
        with self.assertRaisesRegex(CHECKER.GateError, "adds or changes a non-canonical H1"):
            self._write_baseline()

    def test_live_repository_contract_and_wiring(self) -> None:
        errors = CHECKER.audit_repository(
            ROOT,
            ROOT / "scripts/ci/research-digest-id-baseline.json",
        )
        self.assertEqual(errors, [])

        precommit = (ROOT / ".pre-commit-config.yaml").read_text(encoding="utf-8")
        workflow = (ROOT / ".github/workflows/rule-enforcement.yml").read_text(encoding="utf-8")
        command = "python3 -B scripts/ci/check-research-digest-ids.py"
        test_command = "python3 -B scripts/ci/tests/test_research_digest_ids.py"
        self.assertIn(command, precommit)
        self.assertIn(test_command, precommit)
        self.assertIn(command, workflow)
        self.assertIn(test_command, workflow)

    def test_historical_0033_0034_renames_stay_restored(self) -> None:
        research = ROOT / "docs/research"
        self.assertFalse((research / "0033-hip-applicability.md").exists())
        self.assertFalse((research / "0034-ci-pipeline-audit-2026-05.md").exists())
        self.assertTrue((research / "0432-hip-applicability.md").is_file())
        self.assertTrue((research / "0433-ci-pipeline-audit-2026-05.md").is_file())

        expected_targets = {
            "docs/adr/0212-hip-backend-scaffold.md": (
                "0033-hip-applicability.md",
                "0432-hip-applicability.md",
            ),
            "docs/adr/_index_fragments/0212-hip-backend-scaffold.md": (
                "0033-hip-applicability.md",
                "0432-hip-applicability.md",
            ),
            "docs/backends/hip/overview.md": (
                "0033-hip-applicability.md",
                "0432-hip-applicability.md",
            ),
            "docs/research/0086-adr-proposed-status-sweep-2026-05-08.md": (
                "0034-ci-pipeline-audit-2026-05.md",
                "0433-ci-pipeline-audit-2026-05.md",
            ),
            "docs/usage/bd-rate.md": (
                "0034-ci-pipeline-audit-2026-05.md",
                "0433-ci-pipeline-audit-2026-05.md",
            ),
        }
        for relative, (old_target, current_target) in expected_targets.items():
            text = (ROOT / relative).read_text(encoding="utf-8")
            self.assertNotIn(old_target, text)
            self.assertIn(current_target, text)


if __name__ == "__main__":
    unittest.main()
