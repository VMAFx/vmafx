#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Offline planted-defect recall for the Codacy Bandit finding gate."""

from __future__ import annotations

import contextlib
import io
import json
import os
import re
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from identify.identify import tags_from_filename

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.ci import check_bandit_new as check
from scripts.ci.tests.scanner_test_support import (
    assert_push_contract,
    isolate_pre_commit_environment,
    run_push_entry,
    scanner_hooks,
)
from scripts.lib import new_scanner_findings as shared
from scripts.lib.safe_subprocess import run as run_command

BASE = "from pathlib import Path\n\nexe = Path('tool')\n"
UNSAFE = "import subprocess\n\nsubprocess.run([exe], check=False, timeout=5)  # noqa: S603\n"
SAFE = "from scripts.lib.safe_subprocess import run\n\nrun([exe], allowed_executables=(exe,))\n"


class CheckBanditNew(unittest.TestCase):
    def setUp(self) -> None:
        """Require Bandit and create an isolated repository with a baseline and change branch."""
        isolate_pre_commit_environment(self)
        self.assertIsNotNone(shutil.which("bandit"), "bandit is not on PATH")
        temp = tempfile.TemporaryDirectory(prefix="vmafx-bandit-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.git("init", "-q", "-b", "master")
        (self.root / "harness.py").write_text(BASE, encoding="utf-8")
        (self.root / "bandit.yml").write_bytes(check.CONFIG.read_bytes())
        self.git("add", "harness.py", "bandit.yml")
        self.git("commit", "-q", "-m", "base")
        self.git("switch", "-q", "-c", "change")

    def git(self, *args: str, input_text: str | None = None) -> str:
        """Run a fixture repository command, assert success, and return standard output."""
        git = shutil.which("git")
        assert git is not None
        # Drop the caller's repository variables (a hook runs with GIT_DIR and
        # GIT_INDEX_FILE set) so the fixture never touches the caller's repository.
        env = {
            **{k: v for k, v in os.environ.items() if not k.startswith("GIT_")},
            "GIT_CONFIG_GLOBAL": os.devnull,
            "GIT_CONFIG_SYSTEM": os.devnull,
            "GIT_AUTHOR_NAME": "t",
            "GIT_AUTHOR_EMAIL": "t@example.invalid",
            "GIT_COMMITTER_NAME": "t",
            "GIT_COMMITTER_EMAIL": "t@example.invalid",
        }
        proc = run_command(
            [
                git,
                "-c",
                "commit.gpgsign=false",
                "-c",
                f"core.hooksPath={self.root / 'no-hooks'}",
                *args,
            ],
            allowed_executables=(git,),
            cwd=self.root,
            env=env,
            capture_output=True,
            text=True,
            timeout_seconds=60,
            input_data=input_text,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        return proc.stdout

    def run_check(self, name: str, source: str) -> tuple[int, str, str]:
        """Write fixture source and return the check's status, standard output, and errors."""
        (self.root / name).write_text(source, encoding="utf-8")
        out, err = io.StringIO(), io.StringIO()
        cwd = Path.cwd()
        os.chdir(self.root)
        try:
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                status = check.main(["--base-branch", "master", name])
        finally:
            os.chdir(cwd)
        return status, out.getvalue(), err.getvalue()

    def invoke(self, *names: str, base: str | None = "master") -> tuple[int, str, str]:
        """Check selected fixture paths and return status, standard output, and errors."""
        argv = [*(["--base-branch", base] if base else []), *names]
        out, err = io.StringIO(), io.StringIO()
        with (
            contextlib.chdir(self.root),
            contextlib.redirect_stdout(out),
            contextlib.redirect_stderr(err),
        ):
            status = check.main(argv)
        return status, out.getvalue(), err.getvalue()

    def commit_base(self) -> None:
        """Commit fixture changes and make that revision the comparison baseline."""
        self.git("add", ".")
        self.git("commit", "-q", "-m", "baseline")
        self.git("branch", "-f", "master", "HEAD")

    def outgoing_entry(self, **kwargs) -> tuple[int, str, str]:
        """Invoke the parsed push entry using the fixture's default comparison branch."""
        self.git("branch", "-f", "origin/master", "master")
        return run_push_entry(self.root, "bandit", **kwargs)

    def commit_outgoing(self, name: str, source: str) -> None:
        """Commit one source fixture without staging the copied hook implementation."""
        (self.root / name).write_text(source)
        self.git("add", name)
        self.git("commit", "-q", "-m", "outgoing source")

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
        (self.root / "harness.py").write_text("assert True\n")
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
        (self.root / "new.py").write_text(BASE)
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
        (self.root / "harness.py").write_text(BASE + UNSAFE)
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
        (self.root / "harness.py").write_text(BASE + UNSAFE)
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
        (self.root / "harness.py").write_text("assert True\n")
        self.commit_base()
        (self.root / "bandit.yml").write_text("tests: [B101]\n")
        status, out, err = self.invoke("bandit.yml")
        self.assertEqual(status, 1, err)
        self.assertIn("harness.py:1: B101", out)

    def test_base_config_is_used_on_changed_file(self) -> None:
        """A moved assertion newly enabled by B101 is absent from base findings and exits 1."""
        (self.root / "harness.py").write_text("assert True\n")
        self.commit_base()
        (self.root / "bandit.yml").write_text("tests: [B101]\n")
        status, out, err = self.run_check("harness.py", "# moved\nassert True\n")
        self.assertEqual(status, 1, err)
        self.assertIn("B101", out)

    def test_missing_base_config_falls_back_to_current(self) -> None:
        """A missing base configuration uses current rules, leaving an old finding at exit 0."""
        self.git("rm", "bandit.yml")
        (self.root / "harness.py").write_text(BASE + UNSAFE)
        self.commit_base()
        (self.root / "bandit.yml").write_bytes(check.CONFIG.read_bytes())
        self.assertEqual(self.invoke("bandit.yml")[0], 0)

    def test_disabled_rule_does_not_report_old_finding(self) -> None:
        """Disabling B101 suppresses the baseline assertion finding and exits 0."""
        (self.root / "bandit.yml").write_text("tests: [B101]\n")
        (self.root / "harness.py").write_text("assert True\n")
        self.commit_base()
        (self.root / "bandit.yml").write_bytes(check.CONFIG.read_bytes())
        self.assertEqual(self.invoke("bandit.yml")[0], 0)

    def test_config_change_retains_existing_rules(self) -> None:
        """A configuration comment leaves existing findings classified as old and exits 0."""
        (self.root / "harness.py").write_text(BASE + UNSAFE)
        self.commit_base()
        with (self.root / "bandit.yml").open("a") as config:
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
        (self.root / "harness.py").write_text(BASE + UNSAFE)
        self.commit_base()
        self.git("mv", "harness.py", "renamed.py")
        self.git("commit", "-q", "-m", "rename")
        self.git("branch", "outgoing")
        self.git("switch", "-q", "master")
        with mock.patch.dict(os.environ, {"PRE_COMMIT_TO_REF": "outgoing"}):
            self.assertEqual(self.invoke("renamed.py")[0], 0)

    def test_symlink_source_is_not_followed(self) -> None:
        """A source identified as a symlink is skipped and returns 0."""
        (self.root / "harness.py").write_text(BASE + UNSAFE)
        # Model lstat without requiring Windows symlink creation privileges.
        with mock.patch.object(
            Path, "is_symlink", autospec=True, side_effect=lambda path: path.name == "harness.py"
        ):
            self.assertEqual(self.invoke("harness.py")[0], 0)

    def test_push_config_skips_tracked_symlink(self) -> None:
        """A config change skips an outgoing Python symlink instead of parsing its target."""
        blob = self.git("hash-object", "-w", "--stdin", input_text="../module.py").strip()
        self.git("update-index", "--add", "--cacheinfo", f"120000,{blob},linked.py")
        with (self.root / "bandit.yml").open("a") as config:
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
                (self.root / "harness.py").write_text(BASE + UNSAFE)
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
        (self.root / "harness.py").write_text(BASE + UNSAFE)
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
        with (self.root / "bandit.yml").open("a") as config:
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
        (self.root / "harness.py").write_text(BASE + UNSAFE)
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
        source.write_text(BASE + UNSAFE)
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
        (self.root / "unselected.py").write_text(BASE + UNSAFE)
        self.git("add", "unselected.py")
        self.assertEqual(self.invoke("harness.py")[0], 0)

    def test_push_config_uses_outgoing_blob_and_tracked_files(self) -> None:
        """An outgoing rule change exposes a tracked assertion and returns 1."""
        (self.root / "harness.py").write_text("assert True\n")
        self.commit_base()
        (self.root / "bandit.yml").write_text("tests: [B101]\n")
        self.git("add", ".")
        self.git("commit", "-q", "-m", "enable rule")
        self.git("branch", "outgoing")
        self.git("switch", "-q", "master")
        with mock.patch.dict(os.environ, {"PRE_COMMIT_TO_REF": "outgoing"}):
            self.assertEqual(self.invoke("bandit.yml")[0], 1)

    def test_python_hook_types_and_shebang_are_scanned(self) -> None:
        """Selected suffixes and shebangs expose B603; plain text and missing files pass."""
        self.assertIn("python", tags_from_filename("x.pyw"))
        self.assertIn("pyi", tags_from_filename("x.pyi"))
        for name in ("fresh.pyw", "fresh.pyi", "fresh.pyt", "fresh.wsgi", "executable"):
            with self.subTest(name=name):
                source = "#!/usr/bin/env python3\n" + BASE + UNSAFE
                path = self.root / name
                path.write_text(source)
                path.chmod(0o755)
                status, out, err = self.invoke(name)
                self.assertEqual(status, 1, err)
                self.assertIn("B603", out)
        self.assertEqual(self.run_check("plain.txt", BASE + UNSAFE)[0], 0)
        self.assertEqual(self.invoke("missing.py")[0], 0)

    def test_pep263_current_and_base(self) -> None:
        """Encoded source reports a new finding, then passes when the finding exists at base."""
        source = "# coding: latin-1\n# café\n" + BASE + UNSAFE
        path = self.root / "harness.py"
        path.write_bytes(source.encode("latin-1"))
        self.assertEqual(self.invoke("harness.py")[0], 1)
        self.commit_base()
        path.write_bytes((source + "# café moved\n").encode("latin-1"))
        self.assertEqual(self.invoke("harness.py")[0], 0)

    def test_utf8_bom_and_invalid_encoding(self) -> None:
        """A UTF-8 BOM preserves finding detection; an unknown encoding returns 2."""
        path = self.root / "harness.py"
        path.write_bytes((BASE + UNSAFE).encode("utf-8-sig"))
        self.assertEqual(self.invoke("harness.py")[0], 1)
        path.write_bytes(b"# coding: nonexistent-encoding\n")
        self.assertEqual(self.invoke("harness.py")[0], 2)

    def test_literal_metacharacter_base_path(self) -> None:
        """Bracketed filenames retain their base findings when source lines move."""
        for name in ("module[1].py", "module[2].py"):
            (self.root / name).write_text(BASE + UNSAFE)
        self.commit_base()
        for name in ("module[1].py", "module[2].py"):
            with self.subTest(name=name):
                self.assertEqual(self.run_check(name, "# moved\n" + BASE + UNSAFE)[0], 0)

    def test_literal_pathspec_magic_blob(self) -> None:
        # Build the tree directly: ':' and '*' filenames need no filesystem support.
        """A pathspec-like filename returns its literal blob; an absent path returns no bytes."""
        blob = self.git("hash-object", "-w", "--stdin", input_text=BASE + UNSAFE).strip()
        name = ":(glob)module*.py"
        tree = self.git("mktree", "-z", input_text=f"100644 blob {blob}\t{name}\0").strip()
        git = shutil.which("git")
        assert git is not None
        self.assertEqual(shared.base_text(git, self.root, tree, name), (BASE + UNSAFE).encode())
        self.assertEqual(shared.base_text(git, self.root, tree, "absent.py"), b"")

    def test_low_similarity_rename_fails_closed(self) -> None:
        """An unrecognized rename reports its retained B603 finding as new and returns 1."""
        (self.root / "harness.py").write_text(BASE + UNSAFE)
        self.commit_base()
        self.git("mv", "harness.py", "renamed.py")
        status, out, err = self.run_check("renamed.py", BASE + UNSAFE + "# new\n" * 100)
        self.assertEqual(status, 1, err)
        self.assertIn("B603", out)

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
        (self.root / "harness.py").write_text(BASE + UNSAFE, encoding="utf-8")
        self.git("add", "harness.py")
        self.git("commit", "-q", "-m", "unsafe base")
        self.git("switch", "-q", "master")
        self.git("merge", "--ff-only", "change")
        self.git("switch", "-q", "change")
        self.assertEqual(self.run_check("harness.py", "\n" + BASE + UNSAFE)[0], 0)

    def test_renamed_base_finding_is_old(self) -> None:
        """A detected file rename preserves its base finding and returns 0."""
        (self.root / "harness.py").write_text(BASE + UNSAFE, encoding="utf-8")
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
            current.write_text("one line\n", encoding="utf-8")
            scan = mock.Mock(side_effect=AssertionError("scan called"))
            self.assertEqual(shared.new_findings(scan, current, "one line\n"), [])
        scan.assert_not_called()

    def test_one_changed_byte_scans_both_copies(self) -> None:
        """A one-byte source change invokes the scanner for both current and base copies."""
        with tempfile.TemporaryDirectory(prefix="vmafx-scanner-") as temp:
            current = Path(temp) / "changed.py"
            current.write_text("one line!\n", encoding="utf-8")
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


if __name__ == "__main__":
    unittest.main()
