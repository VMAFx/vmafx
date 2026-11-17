#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Push, outgoing-revision, config-baseline, and link-safety tests for check_bandit_new."""

from __future__ import annotations

import os
import shutil
import sys
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.ci import check_bandit_new as check
from scripts.ci.tests.bandit_new_test_support import BASE, UNSAFE, BanditNewTestCase
from scripts.ci.tests.scanner_test_support import assert_push_contract
from scripts.lib import new_scanner_findings as shared


class CheckBanditNew(BanditNewTestCase):
    def test_push_hook_contract(self) -> None:
        """Only the push hook runs unconditionally without positional filenames."""
        assert_push_contract(self, "bandit")

    def test_installed_push_entry_absent_worktree_file(self) -> None:
        """A new-branch push catches its sole new source while master is checked out."""
        self.commit_outgoing("new.py", BASE + UNSAFE)
        self.git("switch", "-q", "master")
        self.assertFalse((self.root / "new.py").exists())
        status, out, err = self.outgoing_entry(from_ref="master", to_ref="change")
        self.assertEqual(status, 1, err)
        self.assertIn("new.py:", out)

    def test_installed_push_entry_docs_followup(self) -> None:
        """A docs-only push still catches findings already on the remote feature tip."""
        self.commit_outgoing("new.py", BASE + UNSAFE)
        self.git("branch", "previous-tip")
        self.commit_outgoing("README.md", "documentation only\n")
        self.git("switch", "-q", "master")
        status, out, err = self.outgoing_entry(from_ref="previous-tip", to_ref="change")
        self.assertEqual(status, 1, err)
        self.assertIn("new.py:", out)

    def test_outgoing_config_scans_unchanged_source(self) -> None:
        """A filename-free outgoing rule change scans unchanged tracked Python source."""
        (self.root / "harness.py").write_text("assert True\n", encoding="utf-8", newline="\n")
        self.commit_base()
        self.commit_outgoing("bandit.yml", "tests: [B101]\n")
        self.git("switch", "-q", "master")
        status, out, err = self.outgoing_entry(to_ref="change")
        self.assertEqual(status, 1, err)
        self.assertIn("harness.py:1: B101", out)

    def test_outgoing_without_new_findings(self) -> None:
        """A safe outgoing addition exits 0 with no findings."""
        self.commit_outgoing("new.py", BASE)
        self.git("switch", "-q", "master")
        status, out, err = self.outgoing_entry(to_ref="change")
        self.assertEqual(status, 0, err)
        self.assertEqual(out, "")

    def test_outgoing_deleted_only(self) -> None:
        """An outgoing deletion needs no source scan and exits 0."""
        self.git("rm", "harness.py")
        self.git("commit", "-q", "-m", "delete only")
        self.git("switch", "-q", "master")
        status, out, err = self.outgoing_entry(to_ref="change")
        self.assertEqual(status, 0, err)
        self.assertEqual(out, "")

    def test_outgoing_rejects_positional_files(self) -> None:
        """Outgoing selection combined with positional files is a usage error."""
        status, _, err = self.outgoing_entry(extra=("new.py",))
        self.assertEqual(status, 2, err)
        self.assertIn("--outgoing cannot be combined", err)

    def test_outgoing_without_merge_base(self) -> None:
        """Unrelated histories fail closed with exit 2."""
        self.git("checkout", "-q", "--orphan", "unrelated")
        self.git("commit", "-q", "-m", "unrelated root")
        status, _, err = self.outgoing_entry(to_ref="unrelated")
        self.assertEqual(status, 2, err)
        self.assertIn("no merge base", err)

    def test_outgoing_manual_reads_head(self) -> None:
        """Manual outgoing mode reads HEAD even when the worktree removes its finding."""
        self.commit_outgoing("new.py", BASE + UNSAFE)
        (self.root / "new.py").write_text(BASE, encoding="utf-8", newline="\n")
        status, out, err = self.outgoing_entry()
        self.assertEqual(status, 1, err)
        self.assertIn("new.py:", out)

    def test_outgoing_rename_keeps_base_finding(self) -> None:
        """A Git-detected outgoing rename retains the base finding despite another checkout."""
        self.commit_outgoing("new.py", BASE + UNSAFE)
        self.git("branch", "-f", "master", "HEAD")
        self.git("mv", "new.py", "renamed.py")
        self.git("commit", "-q", "-m", "rename")
        self.git("switch", "-q", "master")
        status, out, err = self.outgoing_entry(to_ref="change")
        self.assertEqual(status, 0, err)
        self.assertEqual(out, "")

    def test_push_scans_outgoing_blob_with_clean_worktree(self) -> None:
        """A new outgoing finding exits 1 even when the checked-out baseline is clean."""
        (self.root / "harness.py").write_text(BASE + UNSAFE, encoding="utf-8", newline="\n")
        self.git("add", ".")
        self.git("commit", "-q", "-m", "outgoing")
        self.git("branch", "outgoing")
        self.git("switch", "-q", "master")
        with mock.patch.dict(os.environ, {"PRE_COMMIT_TO_REF": "outgoing"}):
            status, out, err = self.invoke("harness.py")
        self.assertEqual(status, 1, err)
        self.assertIn("B603", out)

    def test_push_ignores_dirty_worktree_and_deleted_worktree_file(self) -> None:
        """A clean outgoing blob exits 0 despite local unsafe edits or deletion."""
        self.git("branch", "outgoing")
        with mock.patch.dict(os.environ, {"PRE_COMMIT_TO_REF": "outgoing"}):
            self.assertEqual(self.run_check("harness.py", BASE + UNSAFE)[0], 0)
            (self.root / "harness.py").unlink()
            self.assertEqual(self.invoke("harness.py")[0], 0)

    def test_push_uses_outgoing_merge_base(self) -> None:
        """An outgoing baseline finding exits 0 even with an older revision checked out."""
        (self.root / "harness.py").write_text(BASE + UNSAFE, encoding="utf-8", newline="\n")
        self.commit_base()
        self.git("branch", "outgoing")
        self.git("switch", "-q", "--detach", "master~1")
        with mock.patch.dict(os.environ, {"PRE_COMMIT_TO_REF": "outgoing"}):
            self.assertEqual(self.invoke("harness.py")[0], 0)

    def test_default_branch_symbolic_ref_and_override(self) -> None:
        """The origin HEAD target succeeds; an explicit missing base overrides it and exits 2."""
        self.git("branch", "origin/main", "master")
        self.git("symbolic-ref", "refs/remotes/origin/HEAD", "refs/heads/origin/main")
        self.assertEqual(self.invoke("harness.py", base=None)[0], 0)
        self.assertEqual(self.invoke("harness.py", base="missing")[0], 2)

    def test_default_branch_fallback_and_missing_merge_base(self) -> None:
        """Without origin HEAD, a missing origin/master exits 2; creating it allows exit 0."""
        self.assertEqual(self.invoke("harness.py", base=None)[0], 2)
        self.git("branch", "origin/master", "master")
        self.assertEqual(self.invoke("harness.py", base=None)[0], 0)

    def test_config_enables_rule_on_unchanged_file(self) -> None:
        """Enabling B101 reports the unchanged source's assertion and exits 1."""
        (self.root / "harness.py").write_text("assert True\n", encoding="utf-8", newline="\n")
        self.commit_base()
        (self.root / "bandit.yml").write_text("tests: [B101]\n", encoding="utf-8", newline="\n")
        status, out, err = self.invoke("bandit.yml")
        self.assertEqual(status, 1, err)
        self.assertIn("harness.py:1: B101", out)

    def test_base_config_is_used_on_changed_file(self) -> None:
        """A moved assertion newly enabled by B101 is absent from base findings and exits 1."""
        (self.root / "harness.py").write_text("assert True\n", encoding="utf-8", newline="\n")
        self.commit_base()
        (self.root / "bandit.yml").write_text("tests: [B101]\n", encoding="utf-8", newline="\n")
        status, out, err = self.run_check("harness.py", "# moved\nassert True\n")
        self.assertEqual(status, 1, err)
        self.assertIn("B101", out)

    def test_missing_base_config_falls_back_to_current(self) -> None:
        """A missing base configuration uses current rules, leaving an old finding at exit 0."""
        self.git("rm", "bandit.yml")
        (self.root / "harness.py").write_text(BASE + UNSAFE, encoding="utf-8", newline="\n")
        self.commit_base()
        (self.root / "bandit.yml").write_bytes(check.CONFIG.read_bytes())
        self.assertEqual(self.invoke("bandit.yml")[0], 0)

    def test_disabled_rule_does_not_report_old_finding(self) -> None:
        """Disabling B101 suppresses the baseline assertion finding and exits 0."""
        (self.root / "bandit.yml").write_text("tests: [B101]\n", encoding="utf-8", newline="\n")
        (self.root / "harness.py").write_text("assert True\n", encoding="utf-8", newline="\n")
        self.commit_base()
        (self.root / "bandit.yml").write_bytes(check.CONFIG.read_bytes())
        self.assertEqual(self.invoke("bandit.yml")[0], 0)

    def test_config_change_retains_existing_rules(self) -> None:
        """A configuration comment leaves existing findings classified as old and exits 0."""
        (self.root / "harness.py").write_text(BASE + UNSAFE, encoding="utf-8", newline="\n")
        self.commit_base()
        with (self.root / "bandit.yml").open("a", encoding="utf-8", newline="\n") as config:
            config.write("# same rules\n")
        self.assertEqual(self.invoke("bandit.yml")[0], 0)

    def test_push_deleted_blob_is_not_scanned_from_worktree(self) -> None:
        """An outgoing deletion exits 0 even if unsafe source exists at the local path."""
        self.git("rm", "harness.py")
        self.git("commit", "-q", "-m", "delete")
        self.git("branch", "outgoing")
        with mock.patch.dict(os.environ, {"PRE_COMMIT_TO_REF": "outgoing"}):
            self.assertEqual(self.run_check("harness.py", BASE + UNSAFE)[0], 0)

    def test_push_rename_uses_outgoing_pairing(self) -> None:
        """An outgoing rename preserves the baseline finding match and exits 0."""
        (self.root / "harness.py").write_text(BASE + UNSAFE, encoding="utf-8", newline="\n")
        self.commit_base()
        self.git("mv", "harness.py", "renamed.py")
        self.git("commit", "-q", "-m", "rename")
        self.git("branch", "outgoing")
        self.git("switch", "-q", "master")
        with mock.patch.dict(os.environ, {"PRE_COMMIT_TO_REF": "outgoing"}):
            self.assertEqual(self.invoke("renamed.py")[0], 0)

    def test_symlink_source_is_not_followed(self) -> None:
        """A source identified as a symlink is skipped and returns 0."""
        (self.root / "harness.py").write_text(BASE + UNSAFE, encoding="utf-8", newline="\n")
        # Model lstat without requiring Windows symlink creation privileges.
        with mock.patch.object(
            Path, "is_symlink", autospec=True, side_effect=lambda path: path.name == "harness.py"
        ):
            self.assertEqual(self.invoke("harness.py")[0], 0)

    def test_push_config_skips_tracked_symlink(self) -> None:
        """A config change skips an outgoing Python symlink instead of parsing its target."""
        blob = self.git("hash-object", "-w", "--stdin", input_text="../module.py").strip()
        self.git("update-index", "--add", "--cacheinfo", f"120000,{blob},linked.py")
        with (self.root / "bandit.yml").open("a", encoding="utf-8", newline="\n") as config:
            config.write("# outgoing config\n")
        self.git("add", "bandit.yml")
        self.git("commit", "-q", "-m", "outgoing symlink")
        with mock.patch.dict(os.environ, {"PRE_COMMIT_TO_REF": "HEAD"}):
            status, out, err = self.invoke("bandit.yml")
        self.assertEqual(status, 0, err)
        self.assertEqual(out, "")

    def test_push_regular_and_executable_sources_report_findings(self) -> None:
        """Both regular blob modes retain new outgoing Bandit findings."""
        for executable in (False, True):
            with self.subTest(executable=executable):
                (self.root / "harness.py").write_text(BASE + UNSAFE, encoding="utf-8", newline="\n")
                self.git("add", "harness.py")
                self.git("update-index", "--chmod=+x" if executable else "--chmod=-x", "harness.py")
                self.git("commit", "-q", "-m", "outgoing source")
                with mock.patch.dict(os.environ, {"PRE_COMMIT_TO_REF": "HEAD"}):
                    status, out, err = self.invoke("harness.py")
                self.assertEqual(status, 1, err)
                self.assertIn("harness.py:6: B603", out)

    def test_base_symlink_replaced_by_source_counts_as_absent(self) -> None:
        """Replacing a base symlink scans the new regular file against empty source."""
        blob = self.git("hash-object", "-w", "--stdin", input_text="../module.py").strip()
        self.git("update-index", "--cacheinfo", f"120000,{blob},harness.py")
        self.git("commit", "-q", "-m", "symlink baseline")
        self.git("branch", "-f", "master", "HEAD")
        (self.root / "harness.py").write_text(BASE + UNSAFE, encoding="utf-8", newline="\n")
        self.git("add", "harness.py")
        self.git("commit", "-q", "-m", "replace symlink")
        with mock.patch.dict(os.environ, {"PRE_COMMIT_TO_REF": "HEAD"}):
            status, out, err = self.invoke("harness.py")
        self.assertEqual(status, 1, err)
        self.assertIn("harness.py:6: B603", out)

    def test_push_config_skips_gitlink(self) -> None:
        """A config change skips an outgoing gitlink with a Python filename."""
        commit = self.git("rev-parse", "HEAD").strip()
        self.git("update-index", "--add", "--cacheinfo", f"160000,{commit},linked.py")
        with (self.root / "bandit.yml").open("a", encoding="utf-8", newline="\n") as config:
            config.write("# outgoing config\n")
        self.git("add", "bandit.yml")
        self.git("commit", "-q", "-m", "outgoing gitlink")
        with mock.patch.dict(os.environ, {"PRE_COMMIT_TO_REF": "HEAD"}):
            status, out, err = self.invoke("bandit.yml")
        self.assertEqual(status, 0, err)
        self.assertEqual(out, "")

    def test_base_gitlink_replaced_by_source_counts_as_absent(self) -> None:
        """Replacing a base gitlink reports the regular file's finding without a scan error."""
        commit = self.git("rev-parse", "HEAD").strip()
        self.git("update-index", "--cacheinfo", f"160000,{commit},harness.py")
        self.git("commit", "-q", "-m", "gitlink baseline")
        self.git("branch", "-f", "master", "HEAD")
        (self.root / "harness.py").write_text(BASE + UNSAFE, encoding="utf-8", newline="\n")
        self.git("add", "harness.py")
        self.git("commit", "-q", "-m", "replace gitlink")
        with mock.patch.dict(os.environ, {"PRE_COMMIT_TO_REF": "HEAD"}):
            status, out, err = self.invoke("harness.py")
        self.assertEqual(status, 1, err)
        self.assertIn("B603", out)

    def test_revision_modes_loaded_once_per_revision(self) -> None:
        """Repeated path and byte lookups share one full mode listing per revision."""
        git = shutil.which("git")
        assert git is not None
        with (
            mock.patch.dict(os.environ, {"PRE_COMMIT_TO_REF": "HEAD"}),
            mock.patch.object(shared, "git_output", wraps=shared.git_output) as output,
        ):
            comparison = shared.Comparison(git, self.root, "master")
            for _ in range(2):
                self.assertEqual(comparison.current_bytes("harness.py"), BASE.encode())
                self.assertIn(Path("harness.py"), comparison.tracked_paths())
                self.assertEqual(comparison.old_bytes("harness.py"), BASE.encode())
            listings = [call.args[1] for call in output.call_args_list if "ls-tree" in call.args[1]]
        self.assertCountEqual(
            listings,
            [["ls-tree", "-r", "-z", "HEAD"], ["ls-tree", "-r", "-z", comparison.base]],
        )

    def test_symlink_parent_outside_repository_is_error(self) -> None:
        """A source resolving outside the repository returns 2."""
        original = Path.resolve
        source = self.root / "harness.py"
        source.write_text(BASE + UNSAFE, encoding="utf-8", newline="\n")
        with mock.patch.object(
            Path,
            "resolve",
            autospec=True,
            side_effect=lambda path, strict=False: (
                self.root.parent / "outside.py" if path == source else original(path, strict=strict)
            ),
        ):
            self.assertEqual(self.invoke("harness.py")[0], 2)

    def test_source_path_escape_is_error(self) -> None:
        """A source path containing a parent traversal returns 2."""
        self.assertEqual(self.invoke("../outside.py")[0], 2)

    def test_unchanged_config_does_not_scan_unselected_file(self) -> None:
        """Unchanged configuration leaves an unselected unsafe file unscanned and returns 0."""
        (self.root / "unselected.py").write_text(BASE + UNSAFE, encoding="utf-8", newline="\n")
        self.git("add", "unselected.py")
        self.assertEqual(self.invoke("harness.py")[0], 0)

    def test_push_config_uses_outgoing_blob_and_tracked_files(self) -> None:
        """An outgoing rule change exposes a tracked assertion and returns 1."""
        (self.root / "harness.py").write_text("assert True\n", encoding="utf-8", newline="\n")
        self.commit_base()
        (self.root / "bandit.yml").write_text("tests: [B101]\n", encoding="utf-8", newline="\n")
        self.git("add", ".")
        self.git("commit", "-q", "-m", "enable rule")
        self.git("branch", "outgoing")
        self.git("switch", "-q", "master")
        with mock.patch.dict(os.environ, {"PRE_COMMIT_TO_REF": "outgoing"}):
            self.assertEqual(self.invoke("bandit.yml")[0], 1)


if __name__ == "__main__":
    unittest.main()
