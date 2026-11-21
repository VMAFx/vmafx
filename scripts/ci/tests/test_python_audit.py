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
        self.assertIn("requirements/locks/twine.txt", locks)
        self.assertIn("requirements/locks/package-build.txt", locks)
        self.assertNotIn("requirements/locks/security-audit.txt", locks)
        self.assertEqual(locks, sorted(locks))

    def test_every_manifest_output_is_selected_regardless_of_filename(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = root / "requirements/locks/manifest.json"
            manifest.parent.mkdir(parents=True)
            manifest.write_text(
                json.dumps({"locks": [{"output": "custom/dependencies.txt"}]}), encoding="utf-8"
            )
            self.assertEqual(check_python_audit.project_locks(root), ["custom/dependencies.txt"])

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
        report_json = '{"dependencies": [{"name": "requests", "version": "2.32.5", "vulns": []}]}'
        completed = mock.Mock(returncode=0, stdout=report_json, stderr="")
        with mock.patch.object(check_python_audit, "run_command", return_value=completed) as run:
            report = check_python_audit.run_audit(ROOT, ROOT / "python/requirements-lock.txt")
        self.assertEqual(report, json.loads(report_json))
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
        self.assertIn("expected advisory", str(caught.exception))

    def test_unrelated_advisory_does_not_satisfy_the_self_test(self) -> None:
        report = {
            "dependencies": [
                {"name": "requests", "version": "2.19.0", "vulns": [{"id": "CVE-2099-0001"}]}
            ]
        }
        with mock.patch.object(check_python_audit, "run_audit", return_value=report):
            with self.assertRaises(check_python_audit.AuditError):
                check_python_audit.self_test(
                    ROOT, ROOT / "scripts/ci/fixtures/pip-audit-known-vuln.txt"
                )

    def test_wrong_package_does_not_satisfy_the_self_test(self) -> None:
        report = {
            "dependencies": [
                {"name": "urllib3", "version": "2.0.0", "vulns": [{"id": "CVE-2018-18074"}]}
            ]
        }
        with mock.patch.object(check_python_audit, "run_audit", return_value=report):
            with self.assertRaises(check_python_audit.AuditError):
                check_python_audit.self_test(
                    ROOT, ROOT / "scripts/ci/fixtures/pip-audit-known-vuln.txt"
                )

    def test_ghsa_identity_is_accepted_for_the_planted_advisory(self) -> None:
        report = {
            "dependencies": [
                {"name": "requests", "version": "2.19.0", "vulns": [{"id": "GHSA-x84v-xcm2-53pg"}]}
            ]
        }
        with mock.patch.object(check_python_audit, "run_audit", return_value=report):
            self.assertEqual(
                check_python_audit.self_test(
                    ROOT, ROOT / "scripts/ci/fixtures/pip-audit-known-vuln.txt"
                ),
                "GHSA-x84v-xcm2-53pg",
            )

    def test_empty_dependency_report_fails_closed(self) -> None:
        completed = mock.Mock(returncode=0, stdout='{"dependencies": []}', stderr="")
        with mock.patch.object(check_python_audit, "run_command", return_value=completed):
            with self.assertRaises(check_python_audit.AuditError) as caught:
                check_python_audit.run_audit(ROOT, ROOT / "python/requirements-lock.txt")
        self.assertIn("no dependencies", str(caught.exception))

    def test_planted_audit_runs_before_real_lock_audits(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture = root / "planted.txt"
            fixture.write_text("requests==2.19.0\n", encoding="utf-8")
            lock = root / "project.lock"
            lock.write_text("example==1.0\n", encoding="utf-8")
            manifest = root / "requirements/locks/manifest.json"
            manifest.parent.mkdir(parents=True)
            manifest.write_text(
                json.dumps({"locks": [{"output": "project.lock"}]}), encoding="utf-8"
            )
            ignore = root / "ignore.json"
            ignore.write_text(json.dumps({"schema_version": 1, "exceptions": []}), encoding="utf-8")
            calls: list[Path] = []

            def report(_root: Path, requirement: Path) -> dict[str, object]:
                calls.append(requirement)
                if requirement == fixture:
                    return {
                        "dependencies": [
                            {
                                "name": "requests",
                                "version": "2.19.0",
                                "vulns": [{"id": "CVE-2018-18074"}],
                            }
                        ]
                    }
                return {"dependencies": [{"name": "example", "version": "1.0", "vulns": []}]}

            with mock.patch.object(check_python_audit, "run_audit", side_effect=report):
                result = check_python_audit.main(
                    ["--root", str(root), "--ignore", str(ignore), "--fixture", str(fixture)]
                )
            self.assertEqual(result, 0)
            self.assertEqual(calls, [fixture, lock])


if __name__ == "__main__":
    unittest.main()
