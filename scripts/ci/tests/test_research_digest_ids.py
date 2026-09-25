#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Red-cap contract tests for the research-digest identifier ratchet."""

from __future__ import annotations

import importlib.util
import io
import json
import re
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from types import ModuleType
from typing import Any

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
        self._write_payload_direct()

    def _trusted_file_authority(self) -> Any:
        return CHECKER.DebtAuthority(
            payload=CHECKER.load_baseline(self.baseline),
            revision="f" * 40,
            source="baseline",
        )

    def _git(self, *args: str) -> str:
        result = CHECKER._git(self.root, "-c", "commit.gpgsign=false", *args, check=False)
        if result.returncode != 0:
            self.fail(f"git {' '.join(args)} failed: {result.stderr.decode('utf-8')}")
        return result.stdout.decode("utf-8").strip()

    def _init_git(self) -> None:
        self._git("init", "-b", "master")
        self._git("config", "user.name", "Research fixture")
        self._git("config", "user.email", "research-fixture@example.invalid")

    def _commit(self, message: str = "fixture") -> str:
        self._git("add", ".")
        self._git("commit", "-m", message)
        return self._git("rev-parse", "HEAD")

    def _write_payload_direct(self) -> None:
        payload = CHECKER._baseline_payload(self.root)
        self.baseline.parent.mkdir(parents=True, exist_ok=True)
        self.baseline.write_text(
            json.dumps(payload, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )

    def _commit_trusted_baseline(self) -> str:
        self._digest("1000-alpha.md")
        self._digest("1000-beta.md")
        self._write_payload_direct()
        checker_marker = self.root / "scripts/ci/check-research-digest-ids.py"
        checker_marker.write_text("# trusted checker marker\n", encoding="utf-8")
        return self._commit("trusted baseline")

    def _run_main(self, *args: str) -> tuple[int, str]:
        output = io.StringIO()
        with redirect_stdout(output), redirect_stderr(output):
            result = CHECKER.main(["--root", str(self.root), *args])
        return result, output.getvalue()

    def _hook_files_pattern(self, config: str, hook_id: str) -> re.Pattern[str]:
        marker = f"      - id: {hook_id}\n"
        self.assertIn(marker, config)
        block = config.split(marker, maxsplit=1)[1].split("\n      - id: ", maxsplit=1)[0]
        match = re.search(r"^\s+files: '([^']+)'$", block, flags=re.MULTILINE)
        self.assertIsNotNone(match, f"{hook_id} has no files selector")
        return re.compile(match.group(1))

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
        authority = self._trusted_file_authority()

        self._digest("1000-beta.md")
        with self.assertRaisesRegex(CHECKER.GateError, "adds collision members"):
            CHECKER.write_baseline(self.root, self.baseline, authority)

        path.write_text("# Research-1001: wrong ID\n", encoding="utf-8")
        with self.assertRaisesRegex(CHECKER.GateError, "adds or changes a non-canonical H1"):
            CHECKER.write_baseline(self.root, self.baseline, authority)

    def test_missing_current_baseline_fails_closed(self) -> None:
        self._init_git()
        trusted = self._commit_trusted_baseline()
        self.baseline.unlink()

        result, output = self._run_main("--trusted-ref", trusted)

        self.assertEqual(result, 1)
        self.assertIn("cannot load baseline", output)

    def test_canonical_manual_rewrite_cannot_hide_live_debt(self) -> None:
        self._init_git()
        trusted = self._commit_trusted_baseline()
        self._digest("1000-gamma.md")
        self._write_payload_direct()

        result, output = self._run_main("--trusted-ref", trusted)

        self.assertEqual(result, 1)
        self.assertIn("trusted baseline", output)
        self.assertIn("1000-gamma.md", output)

    def test_manual_baseline_reduction_without_tree_fix_fails(self) -> None:
        self._init_git()
        trusted = self._commit_trusted_baseline()
        payload = CHECKER._baseline_payload(self.root)
        payload["legacy_collisions"] = {}
        self.baseline.write_text(
            json.dumps(payload, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )

        result, output = self._run_main("--trusted-ref", trusted)

        self.assertEqual(result, 1)
        self.assertIn("collision set drift", output)

    def test_ordinary_write_requires_canonical_trusted_baseline(self) -> None:
        self._init_git()
        self._digest("1000-alpha.md")
        trusted = self._commit("pre-ratchet tree")

        result, output = self._run_main("--write", "--trusted-ref", trusted)

        self.assertEqual(result, 1)
        self.assertIn("trusted baseline is missing", output)
        self.assertFalse(self.baseline.exists())

    def test_trusted_checker_without_baseline_is_not_bootstrap_authority(self) -> None:
        self._init_git()
        self._digest("1000-alpha.md")
        checker_marker = self.root / "scripts/ci/check-research-digest-ids.py"
        checker_marker.parent.mkdir(parents=True, exist_ok=True)
        checker_marker.write_text("# trusted checker marker\n", encoding="utf-8")
        trusted = self._commit("checker without baseline")
        self._write_payload_direct()

        result, output = self._run_main("--trusted-ref", trusted)

        self.assertEqual(result, 1)
        self.assertIn("canonical trusted baseline is missing", output)

    def test_initial_adoption_audit_accepts_only_debt_reduction(self) -> None:
        self._init_git()
        self._digest("1000-alpha.md")
        second = self._digest("1000-beta.md")
        trusted = self._commit("pre-ratchet tree")
        second.unlink()
        self._write_payload_direct()

        result, output = self._run_main("--trusted-ref", trusted)

        self.assertEqual(result, 0, output)
        self.assertIn(f"trusted {trusted}", output)

    def test_explicit_bootstrap_requires_immutable_pre_ratchet_ref(self) -> None:
        self._init_git()
        self._digest("1000-alpha.md")
        trusted = self._commit("pre-ratchet tree")

        result, output = self._run_main("--bootstrap-from-ref", trusted)

        self.assertEqual(result, 0, output)
        self.assertTrue(self.baseline.is_file())
        self.baseline.unlink()

        result, output = self._run_main("--bootstrap-from-ref", "master")
        self.assertEqual(result, 1)
        self.assertIn("full 40-character commit", output)

    def test_bootstrap_cannot_absorb_debt_added_after_trusted_ref(self) -> None:
        self._init_git()
        self._digest("1000-alpha.md")
        trusted = self._commit("pre-ratchet tree")
        self._digest("1000-beta.md")

        result, output = self._run_main("--bootstrap-from-ref", trusted)

        self.assertEqual(result, 1)
        self.assertIn("refusing initial baseline debt growth", output)
        self.assertFalse(self.baseline.exists())

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
        self.assertIn(
            "VMAFX_RESEARCH_BASE_REF: ${{ github.event.pull_request.base.sha }}", workflow
        )
        self.assertIn('--trusted-ref "${VMAFX_RESEARCH_BASE_REF}"', workflow)
        self.assertNotIn("--bootstrap-from-ref", workflow)
        self.assertNotIn("--bootstrap-from-ref", precommit)

        expected_triggers = {
            ".github/AGENTS.md",
            ".github/workflows/rule-enforcement.yml",
            ".pre-commit-config.yaml",
            "changelog.d/_pre_fragment_legacy.md",
            "docs/adr/0212-hip-backend-scaffold.md",
            "docs/adr/1335-research-digest-identity-ratchet.md",
            "docs/adr/_index_fragments/0212-hip-backend-scaffold.md",
            "docs/adr/_index_fragments/1335-research-digest-identity-ratchet.md",
            "docs/backends/hip/overview.md",
            "docs/research/0086-adr-proposed-status-sweep-2026-05-08.md",
            "docs/research/0432-hip-applicability.md",
            "docs/research/0433-ci-pipeline-audit-2026-05.md",
            "docs/usage/bd-rate.md",
            "scripts/ci/AGENTS.md",
            "scripts/ci/check-research-digest-ids.py",
            "scripts/ci/research-digest-id-baseline.json",
            "scripts/ci/tests/test_research_digest_ids.py",
        }
        for hook_id in ("check-research-digest-ids", "test-research-digest-ids"):
            pattern = self._hook_files_pattern(precommit, hook_id)
            for relative in expected_triggers:
                self.assertIsNotNone(pattern.fullmatch(relative), f"{hook_id} misses {relative}")

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
            "changelog.d/_pre_fragment_legacy.md": (
                "0033-hip-applicability.md",
                "0432-hip-applicability.md",
            ),
        }
        for relative, (old_target, current_target) in expected_targets.items():
            text = (ROOT / relative).read_text(encoding="utf-8")
            self.assertNotIn(old_target, text)
            self.assertIn(current_target, text)

    def test_train_era_2080_collision_and_h1_debt_stay_repaired(self) -> None:
        research = ROOT / "docs/research"
        self.assertTrue((research / "2080-rust-ci-path-filter-coverage.md").is_file())
        self.assertFalse((research / "2080-hip-float-motion-lifecycle-flush.md").exists())
        self.assertFalse((research / "2080-codeql-python-alerts-triage-2026-09-24.md").exists())

        expected_headings = {
            "1306-drop-nvidia-cuda-base.md": "# Research-1306:",
            "1317-golden-gate-build-isolation.md": "# Research-1317:",
            "2115-hip-float-motion-lifecycle-flush.md": "# Research-2115:",
            "2116-codeql-python-alerts-triage-2026-09-24.md": "# Research-2116:",
        }
        for name, prefix in expected_headings.items():
            text = (research / name).read_text(encoding="utf-8")
            self.assertTrue(CHECKER._first_h1(text).startswith(prefix))


if __name__ == "__main__":
    unittest.main()
