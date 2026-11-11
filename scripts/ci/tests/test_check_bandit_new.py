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

import yaml
from identify.identify import tags_from_filename

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.ci import check_bandit_new as check
from scripts.lib import new_scanner_findings as shared
from scripts.lib.safe_subprocess import run as run_command

BASE = "from pathlib import Path\n\nexe = Path('tool')\n"
UNSAFE = "import subprocess\n\nsubprocess.run([exe], check=False, timeout=5)  # noqa: S603\n"
SAFE = "from scripts.lib.safe_subprocess import run\n\nrun([exe], allowed_executables=(exe,))\n"


class CheckBanditNew(unittest.TestCase):
    def setUp(self) -> None:
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
            [git, "-c", "commit.gpgsign=false", *args],
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
        self.git("add", ".")
        self.git("commit", "-q", "-m", "baseline")
        self.git("branch", "-f", "master", "HEAD")

    def test_push_scans_outgoing_blob_with_clean_worktree(self) -> None:
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
        self.git("branch", "outgoing")
        with mock.patch.dict(os.environ, {"PRE_COMMIT_TO_REF": "outgoing"}):
            self.assertEqual(self.run_check("harness.py", BASE + UNSAFE)[0], 0)
            (self.root / "harness.py").unlink()
            self.assertEqual(self.invoke("harness.py")[0], 0)

    def test_push_uses_outgoing_merge_base(self) -> None:
        (self.root / "harness.py").write_text(BASE + UNSAFE)
        self.commit_base()
        self.git("branch", "outgoing")
        self.git("switch", "-q", "--detach", "master~1")
        with mock.patch.dict(os.environ, {"PRE_COMMIT_TO_REF": "outgoing"}):
            self.assertEqual(self.invoke("harness.py")[0], 0)

    def test_default_branch_symbolic_ref_and_override(self) -> None:
        self.git("branch", "origin/main", "master")
        self.git("symbolic-ref", "refs/remotes/origin/HEAD", "refs/heads/origin/main")
        self.assertEqual(self.invoke("harness.py", base=None)[0], 0)
        self.assertEqual(self.invoke("harness.py", base="missing")[0], 2)

    def test_default_branch_fallback_and_missing_merge_base(self) -> None:
        self.assertEqual(self.invoke("harness.py", base=None)[0], 2)
        self.git("branch", "origin/master", "master")
        self.assertEqual(self.invoke("harness.py", base=None)[0], 0)

    def test_config_enables_rule_on_unchanged_file(self) -> None:
        (self.root / "harness.py").write_text("assert True\n")
        self.commit_base()
        (self.root / "bandit.yml").write_text("tests: [B101]\n")
        status, out, err = self.invoke("bandit.yml")
        self.assertEqual(status, 1, err)
        self.assertIn("harness.py:1: B101", out)

    def test_base_config_is_used_on_changed_file(self) -> None:
        (self.root / "harness.py").write_text("assert True\n")
        self.commit_base()
        (self.root / "bandit.yml").write_text("tests: [B101]\n")
        status, out, err = self.run_check("harness.py", "# moved\nassert True\n")
        self.assertEqual(status, 1, err)
        self.assertIn("B101", out)

    def test_missing_base_config_falls_back_to_current(self) -> None:
        self.git("rm", "bandit.yml")
        (self.root / "harness.py").write_text(BASE + UNSAFE)
        self.commit_base()
        (self.root / "bandit.yml").write_bytes(check.CONFIG.read_bytes())
        self.assertEqual(self.invoke("bandit.yml")[0], 0)

    def test_disabled_rule_does_not_report_old_finding(self) -> None:
        (self.root / "bandit.yml").write_text("tests: [B101]\n")
        (self.root / "harness.py").write_text("assert True\n")
        self.commit_base()
        (self.root / "bandit.yml").write_bytes(check.CONFIG.read_bytes())
        self.assertEqual(self.invoke("bandit.yml")[0], 0)

    def test_config_change_retains_existing_rules(self) -> None:
        (self.root / "harness.py").write_text(BASE + UNSAFE)
        self.commit_base()
        with (self.root / "bandit.yml").open("a") as config:
            config.write("# same rules\n")
        self.assertEqual(self.invoke("bandit.yml")[0], 0)

    def test_push_deleted_blob_is_not_scanned_from_worktree(self) -> None:
        self.git("rm", "harness.py")
        self.git("commit", "-q", "-m", "delete")
        self.git("branch", "outgoing")
        with mock.patch.dict(os.environ, {"PRE_COMMIT_TO_REF": "outgoing"}):
            self.assertEqual(self.run_check("harness.py", BASE + UNSAFE)[0], 0)

    def test_push_rename_uses_outgoing_pairing(self) -> None:
        (self.root / "harness.py").write_text(BASE + UNSAFE)
        self.commit_base()
        self.git("mv", "harness.py", "renamed.py")
        self.git("commit", "-q", "-m", "rename")
        self.git("branch", "outgoing")
        self.git("switch", "-q", "master")
        with mock.patch.dict(os.environ, {"PRE_COMMIT_TO_REF": "outgoing"}):
            self.assertEqual(self.invoke("renamed.py")[0], 0)

    def test_symlink_source_is_not_followed(self) -> None:
        (self.root / "harness.py").write_text(BASE + UNSAFE)
        # Model lstat without requiring Windows symlink creation privileges.
        with mock.patch.object(
            Path, "is_symlink", autospec=True, side_effect=lambda path: path.name == "harness.py"
        ):
            self.assertEqual(self.invoke("harness.py")[0], 0)

    def test_symlink_parent_outside_repository_is_error(self) -> None:
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
        self.assertEqual(self.invoke("../outside.py")[0], 2)

    def test_unchanged_config_does_not_scan_unselected_file(self) -> None:
        (self.root / "unselected.py").write_text(BASE + UNSAFE)
        self.git("add", "unselected.py")
        self.assertEqual(self.invoke("harness.py")[0], 0)

    def test_push_config_uses_outgoing_blob_and_tracked_files(self) -> None:
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
        source = "# coding: latin-1\n# café\n" + BASE + UNSAFE
        path = self.root / "harness.py"
        path.write_bytes(source.encode("latin-1"))
        self.assertEqual(self.invoke("harness.py")[0], 1)
        self.commit_base()
        path.write_bytes((source + "# café moved\n").encode("latin-1"))
        self.assertEqual(self.invoke("harness.py")[0], 0)

    def test_utf8_bom_and_invalid_encoding(self) -> None:
        path = self.root / "harness.py"
        path.write_bytes((BASE + UNSAFE).encode("utf-8-sig"))
        self.assertEqual(self.invoke("harness.py")[0], 1)
        path.write_bytes(b"# coding: nonexistent-encoding\n")
        self.assertEqual(self.invoke("harness.py")[0], 2)

    def test_literal_metacharacter_base_path(self) -> None:
        for name in ("module[1].py", "module[2].py"):
            (self.root / name).write_text(BASE + UNSAFE)
        self.commit_base()
        for name in ("module[1].py", "module[2].py"):
            with self.subTest(name=name):
                self.assertEqual(self.run_check(name, "# moved\n" + BASE + UNSAFE)[0], 0)

    def test_literal_pathspec_magic_blob(self) -> None:
        # Build the tree directly: ':' and '*' filenames need no filesystem support.
        blob = self.git("hash-object", "-w", "--stdin", input_text=BASE + UNSAFE).strip()
        name = ":(glob)module*.py"
        tree = self.git("mktree", "-z", input_text=f"100644 blob {blob}\t{name}\0").strip()
        git = shutil.which("git")
        assert git is not None
        self.assertEqual(shared.base_text(git, self.root, tree, name), (BASE + UNSAFE).encode())
        self.assertEqual(shared.base_text(git, self.root, tree, "absent.py"), b"")

    def test_low_similarity_rename_fails_closed(self) -> None:
        (self.root / "harness.py").write_text(BASE + UNSAFE)
        self.commit_base()
        self.git("mv", "harness.py", "renamed.py")
        status, out, err = self.run_check("renamed.py", BASE + UNSAFE + "# new\n" * 100)
        self.assertEqual(status, 1, err)
        self.assertIn("B603", out)

    def test_hook_selects_configuration_and_stub_files(self) -> None:
        hooks = yaml.safe_load((check.CONFIG.parent / ".pre-commit-config.yaml").read_text())
        hook = next(h for r in hooks["repos"] for h in r["hooks"] if h["id"] == "bandit-new")
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
        root = check.CONFIG.parent
        config = yaml.safe_load((root / ".pre-commit-config.yaml").read_text())
        hook = next(h for r in config["repos"] for h in r["hooks"] if h["id"] == "bandit-new")
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
        status, out, err = self.run_check("harness.py", BASE + UNSAFE)
        self.assertEqual(status, 1, err)
        self.assertIn("harness.py:6: B603:", out)
        self.assertIn("fix each finding", err)
        self.assertIn("scripts.lib.safe_subprocess.run", err)
        self.assertIn("# noqa does not silence Bandit", err)

    def test_moved_base_finding_is_old(self) -> None:
        (self.root / "harness.py").write_text(BASE + UNSAFE, encoding="utf-8")
        self.git("add", "harness.py")
        self.git("commit", "-q", "-m", "unsafe base")
        self.git("switch", "-q", "master")
        self.git("merge", "--ff-only", "change")
        self.git("switch", "-q", "change")
        self.assertEqual(self.run_check("harness.py", "\n" + BASE + UNSAFE)[0], 0)

    def test_renamed_base_finding_is_old(self) -> None:
        (self.root / "harness.py").write_text(BASE + UNSAFE, encoding="utf-8")
        self.git("add", "harness.py")
        self.git("commit", "-q", "-m", "unsafe base")
        self.git("switch", "-q", "master")
        self.git("merge", "--ff-only", "change")
        self.git("switch", "-q", "change")
        self.git("mv", "harness.py", "renamed.py")
        self.assertEqual(self.run_check("renamed.py", BASE + UNSAFE)[0], 0)

    def test_safe_subprocess_passes(self) -> None:
        self.assertEqual(self.run_check("harness.py", BASE + SAFE)[0], 0)

    def test_disabled_b101_passes(self) -> None:
        self.assertEqual(self.run_check("harness.py", BASE + "assert exe\n")[0], 0)

    def test_missing_bandit_is_error(self) -> None:
        with mock.patch.object(check, "tool", side_effect=shared.ToolError("bandit missing")):
            status, _, err = self.run_check("harness.py", BASE)
        self.assertEqual(status, 2)
        self.assertIn("bandit missing", err)

    def test_bandit_json_errors_are_error(self) -> None:
        result = mock.Mock(returncode=0, stdout=json.dumps({"results": [], "errors": ["bad"]}))
        with mock.patch.object(check, "run_command", return_value=result):
            status, _, err = self.run_check("harness.py", BASE + "# changed\n")
        self.assertEqual(status, 2)
        self.assertIn("scan errors", err)

    def test_invalid_json_is_error(self) -> None:
        result = mock.Mock(returncode=0, stdout="not JSON")
        with mock.patch.object(check, "run_command", return_value=result):
            status, _, err = self.run_check("harness.py", BASE + "# changed\n")
        self.assertEqual(status, 2)
        self.assertIn("Bandit failed", err)

    def test_unexpected_bandit_exit_is_error(self) -> None:
        result = mock.Mock(returncode=3, stdout="", stderr="scan failed")
        with mock.patch.object(check, "run_command", return_value=result):
            status, _, err = self.run_check("harness.py", BASE + "# changed\n")
        self.assertEqual(status, 2)
        self.assertIn("exited 3", err)


class CheckSharedScannerFindings(unittest.TestCase):
    def test_git_output_drops_injected_git_configuration(self) -> None:
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
        with tempfile.TemporaryDirectory(prefix="vmafx-scanner-") as temp:
            current = Path(temp) / "same.py"
            current.write_text("one line\n", encoding="utf-8")
            scan = mock.Mock(side_effect=AssertionError("scan called"))
            self.assertEqual(shared.new_findings(scan, current, "one line\n"), [])
        scan.assert_not_called()

    def test_one_changed_byte_scans_both_copies(self) -> None:
        with tempfile.TemporaryDirectory(prefix="vmafx-scanner-") as temp:
            current = Path(temp) / "changed.py"
            current.write_text("one line!\n", encoding="utf-8")
            scan = mock.Mock(return_value=[])
            self.assertEqual(shared.new_findings(scan, current, "one line\n"), [])
        self.assertEqual(scan.call_count, 2)

    def test_non_utf8_file_scans_both_copies(self) -> None:
        with tempfile.TemporaryDirectory(prefix="vmafx-scanner-") as temp:
            current = Path(temp) / "latin1.c"
            current.write_bytes(b"/* caf\xe9 */\n")
            scan = mock.Mock(return_value=[])
            self.assertEqual(shared.new_findings(scan, current, ""), [])
        self.assertEqual(scan.call_count, 2)


if __name__ == "__main__":
    unittest.main()
