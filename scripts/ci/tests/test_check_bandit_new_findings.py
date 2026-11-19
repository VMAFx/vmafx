#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Hook-selection, finding-identity, error, env, and shared tests for check_bandit_new."""

from __future__ import annotations

import io
import json
import os
import re
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.ci import check_bandit_new as check
from scripts.ci.tests.bandit_new_test_support import BASE, SAFE, UNSAFE, BanditNewTestCase
from scripts.ci.tests.scanner_test_support import (
    isolate_pre_commit_environment,
    scanner_hooks,
)
from scripts.lib import new_scanner_findings as shared


class CheckBanditNew(BanditNewTestCase):
    def test_hook_selects_configuration_and_stub_files(self) -> None:
        """The serial hook selects configuration, stubs, extensionless scripts and sources."""
        hook, push = scanner_hooks("bandit")
        self.assertEqual(hook["stages"], ["pre-commit"])
        self.assertEqual(push["stages"], ["pre-push"])
        self.assertEqual(hook.get("types", ["file"]), ["file"])
        self.assertNotIn("types_or", hook)
        # A config change scans the whole tree; parallel workers would repeat it.
        self.assertIs(hook.get("require_serial"), True)
        # pre-commit's selection: re.search(files) and not re.search(exclude).
        files, exclude = hook.get("files", ""), hook.get("exclude", "^$")
        for path in ("bandit.yml", "core/stub.pyi", "scripts/dev/tool", "x.py"):
            with self.subTest(path=path):
                self.assertTrue(re.search(files, path) and not re.search(exclude, path))

    def test_tooling_suite_installs_hook_scanner_pin(self) -> None:
        """Both tooling dependency files contain the hook's single Bandit version pin."""
        root = check.CONFIG.parent
        hook, push = scanner_hooks("bandit")
        self.assertEqual(hook["additional_dependencies"], push["additional_dependencies"])
        pins = [pin for pin in hook["additional_dependencies"] if pin.startswith("bandit==")]
        self.assertEqual(len(pins), 1)
        self.assertIn(
            pins[0], (root / "requirements/locks/tooling-tests.in").read_text().splitlines()
        )
        self.assertIn(
            pins[0] + " \\",
            (root / "requirements/locks/tooling-tests.txt").read_text().splitlines(),
        )

    def test_noqa_s603_is_new(self) -> None:
        """A noqa suppression leaves a new B603 finding active and returns 1 with guidance."""
        status, out, err = self.run_check("harness.py", BASE + UNSAFE)
        self.assertEqual(status, 1, err)
        self.assertIn("harness.py:6: B603:", out)
        self.assertIn("fix each finding", err)
        self.assertIn("scripts.lib.safe_subprocess.run", err)
        self.assertIn("# noqa does not silence Bandit", err)

    def test_moved_base_finding_is_old(self) -> None:
        """Moving an existing base finding to another line returns 0."""
        (self.root / "harness.py").write_text(BASE + UNSAFE, encoding="utf-8", newline="\n")
        self.git("add", "harness.py")
        self.git("commit", "-q", "-m", "unsafe base")
        self.git("switch", "-q", "master")
        self.git("merge", "--ff-only", "change")
        self.git("switch", "-q", "change")
        self.assertEqual(self.run_check("harness.py", "\n" + BASE + UNSAFE)[0], 0)

    def test_renamed_base_finding_is_old(self) -> None:
        """A detected file rename preserves its base finding and returns 0."""
        (self.root / "harness.py").write_text(BASE + UNSAFE, encoding="utf-8", newline="\n")
        self.git("add", "harness.py")
        self.git("commit", "-q", "-m", "unsafe base")
        self.git("switch", "-q", "master")
        self.git("merge", "--ff-only", "change")
        self.git("switch", "-q", "change")
        self.git("mv", "harness.py", "renamed.py")
        self.assertEqual(self.run_check("renamed.py", BASE + UNSAFE)[0], 0)

    def test_safe_subprocess_passes(self) -> None:
        """An allowlisted process call through the shared wrapper returns 0."""
        self.assertEqual(self.run_check("harness.py", BASE + SAFE)[0], 0)

    def test_disabled_b101_passes(self) -> None:
        """An assertion produces no finding while B101 is disabled and returns 0."""
        self.assertEqual(self.run_check("harness.py", BASE + "assert exe\n")[0], 0)

    def test_missing_bandit_is_error(self) -> None:
        """An unavailable Bandit executable returns 2 and reports the missing scanner."""
        with mock.patch.object(shared, "tool", side_effect=shared.ToolError("bandit missing")):
            status, _, err = self.run_check("harness.py", BASE)
        self.assertEqual(status, 2)
        self.assertIn("bandit missing", err)

    def test_bandit_json_errors_are_error(self) -> None:
        """A successful scanner exit with reported scan errors returns 2."""
        result = mock.Mock(returncode=0, stdout=json.dumps({"results": [], "errors": ["bad"]}))
        with mock.patch.object(check, "run_command", return_value=result):
            status, _, err = self.run_check("harness.py", BASE + "# changed\n")
        self.assertEqual(status, 2)
        self.assertIn("scan errors", err)

    def test_invalid_json_is_error(self) -> None:
        """Malformed scanner JSON returns 2 and reports the Bandit failure."""
        result = mock.Mock(returncode=0, stdout="not JSON")
        with mock.patch.object(check, "run_command", return_value=result):
            status, _, err = self.run_check("harness.py", BASE + "# changed\n")
        self.assertEqual(status, 2)
        self.assertIn("Bandit failed", err)

    def test_unexpected_bandit_exit_is_error(self) -> None:
        """An unexpected scanner exit returns 2 and includes its exit status."""
        result = mock.Mock(returncode=3, stdout="", stderr="scan failed")
        with mock.patch.object(check, "run_command", return_value=result):
            status, _, err = self.run_check("harness.py", BASE + "# changed\n")
        self.assertEqual(status, 2)
        self.assertIn("exited 3", err)


class CheckBanditHookEnvironment(unittest.TestCase):
    def test_fixture_ignores_outer_hook_refs(self) -> None:
        """An existing fixture test passes despite caller refs absent from its repository."""
        injected = {
            "PRE_COMMIT_TO_REF": "f" * 40,
            "PRE_COMMIT_FROM_REF": "e" * 40,
        }
        output = io.StringIO()
        with mock.patch.dict(os.environ, injected):
            suite = unittest.TestSuite([CheckBanditNew("test_safe_subprocess_passes")])
            result = unittest.TextTestRunner(stream=output).run(suite)
            self.assertEqual(result.testsRun, 1)
            self.assertTrue(result.wasSuccessful(), output.getvalue())
            self.assertEqual({key: os.environ[key] for key in injected}, injected)

    def test_prefix_boundary_and_cleanup(self) -> None:
        """All hook keys disappear; near-prefix and unrelated keys survive, then keys return."""
        for value in ("", "outer"):
            with (
                self.subTest(value=value),
                mock.patch.dict(
                    os.environ,
                    {"PRE_COMMIT_CUSTOM": value, "PRE_COMMIT": "kept", "SCANNER_TEST": "kept"},
                ),
            ):
                fixture = unittest.TestCase()
                try:
                    isolate_pre_commit_environment(fixture)
                    self.assertFalse(any(key.startswith("PRE_COMMIT_") for key in os.environ))
                    self.assertEqual(os.environ["PRE_COMMIT"], "kept")
                    self.assertEqual(os.environ["SCANNER_TEST"], "kept")
                finally:
                    fixture.doCleanups()
                self.assertEqual(os.environ["PRE_COMMIT_CUSTOM"], value)


class CheckSharedScannerFindings(unittest.TestCase):
    def test_revision_bytes_skips_links_and_missing_paths(self) -> None:
        """Links and absent entries never reach git show, even with a source suffix."""
        modes = {"link.py": "120000", "gitlink.py": "160000"}
        with mock.patch.object(
            shared, "git_bytes", side_effect=AssertionError("link must not reach git show")
        ) as read:
            for name in ("link.py", "gitlink.py", "missing.py"):
                with self.subTest(name=name):
                    self.assertIsNone(
                        shared.revision_bytes("git", Path.cwd(), "outgoing", name, modes=modes)
                    )
                    self.assertEqual(
                        shared.base_text("git", Path.cwd(), "base", name, modes=modes), b""
                    )
        read.assert_not_called()

    def test_revision_modes_reject_malformed_records(self) -> None:
        """Invalid modes, types, object IDs, paths and framing fail closed."""
        oid = "a" * 40
        valid = f"100644 blob {oid}\tsource.py\0"
        malformed = (
            "broken\0",
            valid.replace("100644", "100600"),
            valid.replace("blob", "tree"),
            valid.replace(oid, "invalid"),
            valid.replace("source.py", ""),
            valid.rstrip("\0"),
            valid + "\0",
            valid + valid,
        )
        for output in malformed:
            with (
                self.subTest(output=output),
                mock.patch.object(shared, "git_output", return_value=(0, output)),
                self.assertRaises(shared.ToolError),
            ):
                shared.revision_modes("git", Path.cwd(), "outgoing")

    def test_revision_modes_accept_allowed_entries_and_literal_paths(self) -> None:
        """The mode allow-list retains regular files, links and literal tab/newline paths."""
        entries = {
            "regular.py": ("100644", "blob"),
            "executable.py": ("100755", "blob"),
            "link.py": ("120000", "blob"),
            "gitlink.py": ("160000", "commit"),
            "literal\tline\n.py": ("100644", "blob"),
        }
        output = "".join(
            f"{mode} {kind} {'a' * 40}\t{name}\0" for name, (mode, kind) in entries.items()
        )
        with mock.patch.object(shared, "git_output", return_value=(0, output)):
            self.assertEqual(
                shared.revision_modes("git", Path.cwd(), "outgoing"),
                {name: mode for name, (mode, _) in entries.items()},
            )

    def test_revision_modes_empty_tree_and_git_failure(self) -> None:
        """An empty tree has no entries; a failed tree listing raises ToolError."""
        with mock.patch.object(shared, "git_output", return_value=(0, "")):
            self.assertEqual(shared.revision_modes("git", Path.cwd(), "outgoing"), {})
        with (
            mock.patch.object(shared, "git_output", return_value=(1, "")),
            self.assertRaises(shared.ToolError),
        ):
            shared.revision_modes("git", Path.cwd(), "outgoing")

    def test_git_output_drops_injected_git_configuration(self) -> None:
        """Repository overrides are removed from the child environment; unrelated values remain."""
        injected = {
            "GIT_INDEX_FILE": "/wrong/index",
            "GIT_DIR": "/wrong/repository",
            "SAFE_ENV": "kept",
        }
        result = mock.Mock(returncode=0, stdout=b"repository config")
        with (
            mock.patch("scripts.lib.new_scanner_findings.os.environ", injected),
            mock.patch.object(shared, "run_command", return_value=result) as run,
        ):
            self.assertEqual(
                shared.git_output("git", ["rev-parse", "--show-toplevel"], Path.cwd()),
                (0, "repository config"),
            )
        self.assertEqual(run.call_args.kwargs["env"], {"SAFE_ENV": "kept"})

    def test_identical_text_skips_both_scans(self) -> None:
        """Identical current and base bytes return no findings without invoking the scanner."""
        with tempfile.TemporaryDirectory(prefix="vmafx-scanner-") as temp:
            current = Path(temp) / "same.py"
            current.write_text("one line\n", encoding="utf-8", newline="\n")
            scan = mock.Mock(side_effect=AssertionError("scan called"))
            self.assertEqual(shared.new_findings(scan, current, "one line\n"), [])
        scan.assert_not_called()

    def test_one_changed_byte_scans_both_copies(self) -> None:
        """A one-byte source change invokes the scanner for both current and base copies."""
        with tempfile.TemporaryDirectory(prefix="vmafx-scanner-") as temp:
            current = Path(temp) / "changed.py"
            current.write_text("one line!\n", encoding="utf-8", newline="\n")
            scan = mock.Mock(return_value=[])
            self.assertEqual(shared.new_findings(scan, current, "one line\n"), [])
        self.assertEqual(scan.call_count, 2)

    def test_non_utf8_file_scans_both_copies(self) -> None:
        """Non-UTF-8 source bytes reach both scans without requiring text decoding."""
        with tempfile.TemporaryDirectory(prefix="vmafx-scanner-") as temp:
            current = Path(temp) / "latin1.c"
            current.write_bytes(b"/* caf\xe9 */\n")
            scan = mock.Mock(return_value=[])
            self.assertEqual(shared.new_findings(scan, current, ""), [])
        self.assertEqual(scan.call_count, 2)

    def test_added_findings_preserves_multiset_counts(self) -> None:
        """Shared multiset diff drops matched base findings and retains excess occurrences."""
        old = [(2, "B603", "subprocess.run(cmd)"), (8, "B101", "assert ok")]
        current = [
            (5, "B603", "subprocess.run(cmd)"),
            (9, "B603", "subprocess.run(cmd)"),
            (12, "B101", "assert ok"),
        ]
        self.assertEqual(shared.added_findings(current, old), [(9, "B603", "subprocess.run(cmd)")])


if __name__ == "__main__":
    unittest.main()
