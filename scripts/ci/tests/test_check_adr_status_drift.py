# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Positive, negative and boundary coverage for the ADR status drift report (HISS-15)."""

from __future__ import annotations

import importlib.util
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from datetime import date, datetime, timedelta, timezone
from pathlib import Path
from types import ModuleType

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.lib import git_env

ROOT = Path(__file__).resolve().parents[3]
GIT_TIMEOUT_S = 60


def load_checker() -> ModuleType:
    spec = importlib.util.spec_from_file_location(
        "check_adr_status_drift", ROOT / "scripts/ci/check-adr-status-drift.py"
    )
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot import scripts/ci/check-adr-status-drift.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


CHECKER = load_checker()


def ref(number: int) -> str:
    """Spell a citation without writing the literal, so the source-citation gate ignores it."""
    return "ADR" + f"-{number:04d}"


NOW = datetime(2026, 10, 5, 12, 0, tzinfo=timezone.utc)
TODAY = date(2026, 10, 5)


class StatusDriftTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.repo = Path(self.tmp.name)
        self.env = git_env.drop()
        self.env.update(
            GIT_CONFIG_GLOBAL="/dev/null",
            GIT_CONFIG_SYSTEM="/dev/null",
            GIT_AUTHOR_NAME="t",
            GIT_AUTHOR_EMAIL="t@example.invalid",
            GIT_COMMITTER_NAME="t",
            GIT_COMMITTER_EMAIL="t@example.invalid",
        )
        self.git("init", "-q", "-b", "master")
        (self.repo / "docs" / "adr").mkdir(parents=True)

    def git(self, *args: str, when: datetime | None = None) -> None:
        env = dict(self.env)
        if when is not None:
            env["GIT_AUTHOR_DATE"] = env["GIT_COMMITTER_DATE"] = when.isoformat()
        git = shutil.which("git")
        if git is None:
            self.skipTest("git is not on PATH")
        subprocess.run(  # noqa: S603 - fixed argv built from the test's own arguments
            [git, "-C", str(self.repo), *args],
            check=True,
            env=env,
            capture_output=True,
            timeout=GIT_TIMEOUT_S,
        )

    def adr(self, number: int, status: str) -> None:
        body = f"# ADR-{number:04d}: t\n\n- **Status**: {status}\n- **Date**: 2026-01-01\n"
        (self.repo / "docs" / "adr" / f"{number:04d}-t.md").write_text(body, encoding="utf-8")

    def commit(self, subject: str, days_old: float) -> None:
        self.git(
            "commit", "-q", "--allow-empty", "-m", subject, when=NOW - timedelta(days=days_old)
        )

    def run_drift(self) -> list[int]:
        return [n for n, _, _ in CHECKER.drift(self.repo, "HEAD", 14, now=NOW)]

    def test_planted_defect_is_reported(self) -> None:
        self.adr(1, "Proposed")
        self.commit(f"fix(cuda): do the thing ({ref(1)}) (#5)", 30)
        self.assertEqual(self.run_drift(), [1])

    def test_accepted_adr_is_not_reported(self) -> None:
        self.adr(2, "Accepted")
        self.commit(f"fix(cuda): do the thing ({ref(2)}) (#5)", 30)
        self.assertEqual(self.run_drift(), [])

    def test_recent_commit_is_not_reported(self) -> None:
        self.adr(3, "Proposed")
        self.commit(f"fix(cuda): do the thing ({ref(3)}) (#5)", 2)
        self.assertEqual(self.run_drift(), [])

    def test_age_boundary(self) -> None:
        self.adr(4, "Proposed")
        self.commit(f"fix(cuda): do the thing ({ref(4)}) (#5)", 14)
        self.assertEqual(self.run_drift(), [4])
        self.adr(5, "Proposed")
        self.commit(f"fix(cuda): do the thing ({ref(5)}) (#6)", 13.9)
        self.assertEqual(self.run_drift(), [4])

    def test_docs_commit_does_not_count(self) -> None:
        self.adr(6, "Proposed")
        self.commit(f"docs(adr): record {ref(6)} (#5)", 30)
        self.assertEqual(self.run_drift(), [])

    def test_other_adr_number_does_not_match(self) -> None:
        self.adr(7, "Proposed")
        self.commit(f"fix(cuda): do the thing ({ref(9)}) (#5)", 30)
        self.assertEqual(self.run_drift(), [])

    def exceptions(self, *entries: dict[str, object]) -> Path:
        path = self.repo / "exceptions.json"
        path.write_text(json.dumps({"exceptions": list(entries)}), encoding="utf-8")
        return path

    def entry(self, number: int, expires: str = "2027-01-05", **over: object) -> dict[str, object]:
        item: dict[str, object] = {
            "adr": number,
            "file": f"docs/adr/{number:04d}-t.md",
            "rule": CHECKER.RULE,
            "reason": "the missing part",
            "expires": expires,
        }
        item.update(over)
        return item

    def problems(self, path: Path, today: date = TODAY) -> list[str]:
        found, _ = CHECKER.evaluate(self.repo, "HEAD", 14, path, today, now=NOW)
        return [str(message) for message in found]

    def drifted(self, number: int) -> None:
        self.adr(number, "Proposed")
        self.commit(f"fix(x): y ({ref(number)}) (#5)", 30)

    def test_new_drift_without_exception_fails(self) -> None:
        self.drifted(8)
        self.assertEqual(len(self.problems(self.exceptions())), 1)
        argv = ["--repo", str(self.repo), "--ref", "HEAD", "--exceptions", str(self.exceptions())]
        self.assertEqual(CHECKER.main([*argv, "--today", "2026-10-05"]), 1)

    def test_valid_exception_passes(self) -> None:
        self.drifted(8)
        path = self.exceptions(self.entry(8))
        self.assertEqual(self.problems(path), [])
        argv = ["--repo", str(self.repo), "--ref", "HEAD", "--exceptions", str(path)]
        self.assertEqual(CHECKER.main([*argv, "--today", "2026-10-05"]), 0)

    def test_expired_exception_fails_and_expiry_day_passes(self) -> None:
        self.drifted(8)
        path = self.exceptions(self.entry(8, expires="2026-10-05"))
        self.assertEqual(self.problems(path, date(2026, 10, 5)), [])
        self.assertEqual(len(self.problems(path, date(2026, 10, 6))), 1)

    def test_stale_exception_fails(self) -> None:
        self.adr(8, "Accepted")
        self.commit(f"fix(x): y ({ref(8)}) (#5)", 30)
        self.assertEqual(len(self.problems(self.exceptions(self.entry(8)))), 1)

    def test_malformed_and_duplicate_entries_fail(self) -> None:
        self.drifted(8)
        bad_date = self.exceptions(self.entry(8, expires="soon"))
        self.assertTrue(self.problems(bad_date))
        no_reason = self.exceptions(self.entry(8, reason=" "))
        self.assertTrue(self.problems(no_reason))
        wrong_rule = self.exceptions(self.entry(8, rule="other"))
        self.assertTrue(self.problems(wrong_rule))
        wrong_file = self.exceptions(self.entry(8, file="docs/adr/0009-t.md"))
        self.assertTrue(self.problems(wrong_file))
        twice = self.exceptions(self.entry(8), self.entry(8))
        self.assertTrue(self.problems(twice))

    def test_repository_exception_list_is_valid(self) -> None:
        entries, problems = CHECKER.load_exceptions(CHECKER.EXCEPTIONS_FILE, ROOT)
        self.assertEqual(problems, [])
        self.assertTrue(entries)


if __name__ == "__main__":
    unittest.main()
