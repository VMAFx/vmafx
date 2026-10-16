# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Positive, negative and boundary cases for lint_exceptions.py (HISS-15)."""

from __future__ import annotations

import datetime as dt
import importlib.util
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
_SPEC = importlib.util.spec_from_file_location(
    "lint_exceptions", ROOT / "scripts/ci/lint_exceptions.py"
)
assert _SPEC is not None and _SPEC.loader is not None
le = importlib.util.module_from_spec(_SPEC)
sys.modules["lint_exceptions"] = le  # the dataclass needs its module registered
_SPEC.loader.exec_module(le)

TODAY = dt.date(2026, 10, 5)
ENTRY = """\
[[exception]]
path = "a/b.c"
reason = "mirror"
expires = {expires}
"""


class LintExceptionsTests(unittest.TestCase):
    """Parse, validate and apply the list against a throw-away root."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        (self.root / le.LIST_DIR).mkdir(parents=True)

    def write(self, rule: str, text: str) -> None:
        (self.root / le.LIST_DIR / f"{rule}.toml").write_text(
            textwrap.dedent(text), encoding="utf-8"
        )

    def findings(self, tracked: set[str] | None = None) -> list[str]:
        entries, bad = le.load(self.root)
        found: list[str] = bad + le.validate(entries, frozenset(tracked or {"a/b.c"}), TODAY)
        return found

    def test_live_entry_is_clean_and_drops_its_file(self) -> None:
        self.write("spdx", ENTRY.format(expires="2026-12-31"))
        self.assertEqual(self.findings(), [])
        self.assertEqual(le.still_read("spdx", ["a/b.c", "x.c"], self.root, TODAY), ["x.c"])

    def test_exception_is_per_rule(self) -> None:
        self.write("spdx", ENTRY.format(expires="2026-12-31"))
        self.assertEqual(le.still_read("copyright", ["a/b.c"], self.root, TODAY), ["a/b.c"])

    def test_expired_entry_no_longer_holds_and_is_reported(self) -> None:
        self.write("spdx", ENTRY.format(expires="2026-10-04"))
        self.assertEqual(le.still_read("spdx", ["a/b.c"], self.root, TODAY), ["a/b.c"])
        self.assertIn("expired on 2026-10-04", "\n".join(self.findings()))

    def test_entry_expiring_today_still_holds(self) -> None:
        self.write("spdx", ENTRY.format(expires="2026-10-05"))
        self.assertEqual(le.still_read("spdx", ["a/b.c"], self.root, TODAY), [])
        self.assertEqual(self.findings(), [])

    def test_expiry_beyond_the_cap_is_refused(self) -> None:
        self.write("spdx", ENTRY.format(expires="2030-01-01"))
        self.assertIn("days out", "\n".join(self.findings()))

    def test_missing_field_is_refused(self) -> None:
        self.write("spdx", '[[exception]]\npath = "a/b.c"\nexpires = 2026-12-31\n')
        self.assertIn("needs path, reason, expires", "\n".join(self.findings()))

    def test_expiry_must_be_a_date(self) -> None:
        self.write("spdx", ENTRY.format(expires='"never"'))
        self.assertIn("expires a TOML date", "\n".join(self.findings()))

    def test_pattern_and_untracked_paths_are_refused(self) -> None:
        self.write("spdx", ENTRY.format(expires="2026-12-31").replace("a/b.c", "a/*.c"))
        self.assertIn("not a pattern", "\n".join(self.findings({"a/x.c"})))
        self.write("spdx", ENTRY.format(expires="2026-12-31"))
        self.assertIn("not a tracked file", "\n".join(self.findings({"other.c"})))

    def test_rule_must_match_the_file_name(self) -> None:
        self.write("spdx", ENTRY.format(expires="2026-12-31") + 'rule = "copyright"\n')
        self.assertIn("differs from the file name", "\n".join(self.findings()))

    def test_duplicate_is_refused(self) -> None:
        self.write("spdx", ENTRY.format(expires="2026-12-31") * 2)
        self.assertIn("declared twice", "\n".join(self.findings()))

    def test_two_jobs_of_one_workflow_are_two_entries(self) -> None:
        one = ENTRY.format(expires="2026-12-31")
        self.write("hosted-runner-label", one + 'job = "a"\n' + one + 'job = "b"\n')
        self.assertEqual(self.findings(), [])

    def test_one_job_declared_twice_is_refused(self) -> None:
        one = ENTRY.format(expires="2026-12-31") + 'job = "a"\n'
        self.write("hosted-runner-label", one * 2)
        self.assertIn("hosted-runner-label: a/b.c (a): declared twice", self.findings())

    def test_entries_without_the_part_key_are_one_entry_per_file(self) -> None:
        self.write("hosted-runner-label", ENTRY.format(expires="2026-12-31") * 2)
        self.assertIn("hosted-runner-label: a/b.c: declared twice", self.findings())

    def test_part_key_belongs_to_its_rule_only(self) -> None:
        one = ENTRY.format(expires="2026-12-31")
        self.write("spdx", one + 'job = "a"\n' + one + 'job = "b"\n')
        self.assertIn("spdx: a/b.c: declared twice", self.findings())

    def test_unparseable_file_is_refused(self) -> None:
        self.write("spdx", "[[exception\n")
        self.assertIn("unreadable", "\n".join(self.findings()))

    def test_absolute_path_argument_resolves_under_the_root(self) -> None:
        self.write("spdx", ENTRY.format(expires="2026-12-31"))
        absolute = str(self.root.resolve() / "a/b.c")
        self.assertEqual(le.still_read("spdx", [absolute], self.root.resolve(), TODAY), [])

    def test_no_files_gives_no_output(self) -> None:
        self.assertEqual(le.still_read("spdx", [], self.root, TODAY), [])


class OsvScannerTests(unittest.TestCase):
    """Validate root osv-scanner.toml against a throw-away root."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def write_osv(self, text: str) -> None:
        (self.root / le.OSV_FILE).write_text(textwrap.dedent(text), encoding="utf-8")

    def test_absent_file_gives_no_findings(self) -> None:
        self.assertEqual(le.osv_findings(self.root, TODAY), [])

    def test_live_entry_is_clean(self) -> None:
        self.write_osv("""\
            [[IgnoredVulns]]
            id = "GO-2026-5932"
            reason = "unreachable"
            ignoreUntil = 2026-12-31
        """)
        self.assertEqual(le.osv_findings(self.root, TODAY), [])

    def test_expired_entry_is_reported(self) -> None:
        yesterday = TODAY - dt.timedelta(days=1)
        self.write_osv(f"""\
            [[IgnoredVulns]]
            id = "GO-2026-5932"
            reason = "unreachable"
            ignoreUntil = {yesterday}
        """)
        findings = le.osv_findings(self.root, TODAY)
        self.assertEqual(len(findings), 1)
        self.assertIn(f"osv-scanner.toml GO-2026-5932: expired on {yesterday}", findings[0])

    def test_boundary_today_is_clean(self) -> None:
        self.write_osv(f"""\
            [[IgnoredVulns]]
            id = "GO-2026-5932"
            reason = "unreachable"
            ignoreUntil = {TODAY}
        """)
        self.assertEqual(le.osv_findings(self.root, TODAY), [])

    def test_missing_ignore_until_gives_needs_finding(self) -> None:
        self.write_osv("""\
            [[IgnoredVulns]]
            id = "GO-2026-5932"
            reason = "unreachable"
        """)
        findings = le.osv_findings(self.root, TODAY)
        self.assertEqual(len(findings), 1)
        self.assertIn("needs id, reason, ignoreUntil (a TOML date)", findings[0])

    def test_quoted_string_date_gives_needs_finding(self) -> None:
        self.write_osv("""\
            [[IgnoredVulns]]
            id = "GO-2026-5932"
            reason = "unreachable"
            ignoreUntil = "2026-12-31"
        """)
        findings = le.osv_findings(self.root, TODAY)
        self.assertEqual(len(findings), 1)
        self.assertIn("needs id, reason, ignoreUntil (a TOML date)", findings[0])

    def test_duplicate_id_is_refused(self) -> None:
        self.write_osv("""\
            [[IgnoredVulns]]
            id = "GO-2026-5932"
            reason = "unreachable"
            ignoreUntil = 2026-12-31

            [[IgnoredVulns]]
            id = "GO-2026-5932"
            reason = "duplicate"
            ignoreUntil = 2026-12-31
        """)
        findings = le.osv_findings(self.root, TODAY)
        self.assertIn("osv-scanner.toml GO-2026-5932: declared twice", findings)

    def test_expiry_beyond_cap_is_refused(self) -> None:
        self.write_osv("""\
            [[IgnoredVulns]]
            id = "GO-2026-5932"
            reason = "unreachable"
            ignoreUntil = 2030-01-01
        """)
        findings = le.osv_findings(self.root, TODAY)
        self.assertEqual(len(findings), 1)
        self.assertIn("more than 400 days out", findings[0])


class RepositoryListTests(unittest.TestCase):
    """The tracked list passes, and a stale or expired entry fails the CLI."""

    def run_cli(self, *args: str, today: str | None = None) -> subprocess.CompletedProcess[str]:
        env = {"PATH": "/usr/bin:/bin:/usr/local/bin"}
        if today:
            env["LINT_EXCEPTIONS_TODAY"] = today
        return subprocess.run(  # noqa: S603 -- fixed interpreter and script argv
            [sys.executable, str(ROOT / "scripts/ci/lint_exceptions.py"), *args],
            capture_output=True,
            text=True,
            check=False,
            env=env,
            cwd=ROOT,
            timeout=120,
        )

    def test_tracked_list_passes(self) -> None:
        res = self.run_cli("check")
        self.assertEqual(res.returncode, 0, res.stderr)

    def test_the_cli_fails_once_the_entries_expire(self) -> None:
        res = self.run_cli("check", today="2099-01-01")
        self.assertEqual(res.returncode, 1)
        self.assertIn("expired on", res.stderr)

    def test_host_c_list_names_every_pelorus_mirror_file(self) -> None:
        mirror = [
            line
            for line in (ROOT / "scripts/ci/pelorus-mirror-paths.txt").read_text().splitlines()
            if line and not line.startswith("#")
        ]
        self.assertEqual(len(mirror), 10)
        entries, bad = le.load(ROOT)
        self.assertEqual(bad, [])
        listed = {e.path for e in entries if e.rule == "host-c-cpp"}
        self.assertTrue(set(mirror) <= listed)
        planted = "core/src/interop/planted_unlisted.c"
        self.assertEqual(le.still_read("host-c-cpp", [*mirror, planted]), [planted])

    def test_unknown_command_is_a_usage_error(self) -> None:
        self.assertEqual(self.run_cli("bogus").returncode, 2)


if __name__ == "__main__":
    unittest.main()
