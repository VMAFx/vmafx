#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The Python dependency audit has a planted advisory and dated exceptions (issue 2749)."""

from __future__ import annotations

import json
import shutil
import sys
import tempfile
import unittest
from datetime import date, timedelta
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.ci import check_python_audit  # noqa: E402


class LockSet(unittest.TestCase):
    def test_manifest_locks_exclude_the_audit_tool(self) -> None:
        locks = check_python_audit.project_locks(ROOT)
        self.assertGreaterEqual(len(locks), 5)
        self.assertIn("python/requirements-lock.txt", locks)
        self.assertIn("ai/requirements-runtime-lock.txt", locks)
        self.assertNotIn("requirements/locks/security-audit.txt", locks)
        self.assertEqual(locks, sorted(locks))

    def test_fixture_pins_an_advisory(self) -> None:
        text = (ROOT / "scripts/ci/fixtures/pip-audit-known-vuln.txt").read_text(encoding="utf-8")
        self.assertIn("requests==2.19.0", text)
        self.assertIn("CVE-2018-18074", text)


class Exceptions(unittest.TestCase):
    def write(self, exceptions: list[dict[str, str]]) -> Path:
        directory = Path(tempfile.mkdtemp(prefix="pip-audit-"))
        path = directory / "ignore.json"
        path.write_text(
            json.dumps({"schema_version": 1, "exceptions": exceptions}), encoding="utf-8"
        )
        self.addCleanup(shutil.rmtree, directory)
        return path

    def entry(self, expires: date) -> dict[str, str]:
        return {
            "file": "python/requirements-lock.txt",
            "package": "requests",
            "id": "CVE-2018-18074",
            "reason": "test",
            "expires": expires.isoformat(),
        }

    def test_expired_entry_is_refused(self) -> None:
        path = self.write([self.entry(date.today() - timedelta(days=1))])
        with self.assertRaises(check_python_audit.AuditError) as caught:
            check_python_audit.load_ignore(path)
        self.assertIn("expired", str(caught.exception))

    def test_valid_entry_is_returned(self) -> None:
        path = self.write([self.entry(date.today() + timedelta(days=30))])
        self.assertEqual(len(check_python_audit.load_ignore(path)), 1)

    def test_matching_finding_is_retained(self) -> None:
        exception = self.entry(date.today() + timedelta(days=30))
        used: set[int] = set()
        findings = check_python_audit.collect_findings(
            "python/requirements-lock.txt",
            {
                "dependencies": [
                    {
                        "name": "requests",
                        "version": "2.19.0",
                        "vulns": [{"id": "CVE-2018-18074"}],
                    }
                ]
            },
            used,
            [exception],
        )
        self.assertEqual(findings, [])
        self.assertEqual(used, {0})

    def test_unlisted_finding_is_a_finding(self) -> None:
        findings = check_python_audit.collect_findings(
            "python/requirements-lock.txt",
            {
                "dependencies": [
                    {"name": "requests", "version": "2.19.0", "vulns": [{"id": "CVE-2018-18074"}]}
                ]
            },
            set(),
            [],
        )
        self.assertIn("CVE-2018-18074", findings[0])

    def test_skipped_dependency_is_an_error(self) -> None:
        with self.assertRaises(check_python_audit.AuditError) as caught:
            check_python_audit.collect_findings(
                "python/requirements-lock.txt",
                {
                    "dependencies": [
                        {
                            "name": "local-package",
                            "version": "0.0.0",
                            "skip_reason": "could not resolve",
                            "vulns": [],
                        }
                    ]
                },
                set(),
                [],
            )
        self.assertIn("skipped", str(caught.exception))


class Runner(unittest.TestCase):
    def test_lock_audit_never_uses_pip_resolution(self) -> None:
        completed = mock.Mock(returncode=0, stdout='{"dependencies": []}', stderr="")
        with mock.patch.object(check_python_audit, "run_command", return_value=completed) as run:
            report = check_python_audit.run_audit(ROOT, ROOT / "python/requirements-lock.txt")
        self.assertEqual(report, {"dependencies": []})
        command = run.call_args.args[0]
        self.assertIn("--disable-pip", command)
        self.assertIn("--no-deps", command)


class SelfTest(unittest.TestCase):
    def test_a_planted_advisory_is_required(self) -> None:
        with mock.patch.object(
            check_python_audit,
            "run_audit",
            return_value={
                "dependencies": [
                    {"name": "requests", "version": "2.19.0", "vulns": [{"id": "CVE-2018-18074"}]}
                ]
            },
        ):
            self.assertIn(
                "CVE-2018-18074",
                check_python_audit.self_test(
                    ROOT, ROOT / "scripts/ci/fixtures/pip-audit-known-vuln.txt"
                ),
            )

    def test_no_advisory_fails_the_self_test(self) -> None:
        with mock.patch.object(
            check_python_audit,
            "run_audit",
            return_value={"dependencies": [{"name": "requests", "version": "2.19.0", "vulns": []}]},
        ):
            with self.assertRaises(check_python_audit.AuditError) as caught:
                check_python_audit.self_test(
                    ROOT, ROOT / "scripts/ci/fixtures/pip-audit-known-vuln.txt"
                )
        self.assertIn("no advisory", str(caught.exception))


if __name__ == "__main__":
    unittest.main()
