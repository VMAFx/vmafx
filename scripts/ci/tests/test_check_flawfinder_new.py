#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Planted-defect recall for scripts/ci/check_flawfinder_new.py.

Each case builds a throwaway git repository with a base commit on `master`,
changes a C file on a branch and runs the check against `master`:

- an added `fopen()` (the #3216 Codacy finding) is reported, exit 1;
- a finding the base already carries passes when its line moves;
- reading an input through `fdopen()` on a descriptor passes;
- a new file with a finding is reported;
- a renamed file keeps its base findings (compared with its old path);
- a missing flawfinder is a tool error, exit 2, never a pass.
"""

from __future__ import annotations

import contextlib
import io
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from identify import extensions  # pre-commit's file classifier

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.ci import check_flawfinder_new as check
from scripts.ci.tests.scanner_test_support import (
    assert_push_contract,
    isolate_pre_commit_environment,
    run_push_entry,
)
from scripts.lib.safe_subprocess import run as run_command

BASE = """\
#include <stdio.h>

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    return f == NULL || argc < 2;
}
"""
MOVED = "/* moved down */\n\n" + BASE
ADDED = BASE.replace(
    "    return f == NULL",
    '    FILE *g = fopen(argv[2], "rb");\n    return g == NULL || f == NULL',
)
FDOPEN = BASE.replace(
    "    return f == NULL",
    '    FILE *g = fdopen(3, "rb");\n    return g == NULL || f == NULL',
)


class CheckFlawfinderNew(unittest.TestCase):
    def setUp(self) -> None:
        """Isolate hook variables and create a repository with a baseline and change branch."""
        isolate_pre_commit_environment(self)
        # No skip: the pre-commit hook installs flawfinder, and a missing one
        # fails here instead of passing silently.
        self.assertIsNotNone(shutil.which("flawfinder"), "flawfinder is not on PATH")
        temp = tempfile.TemporaryDirectory(prefix="vmafx-flawfinder-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.git("init", "-q", "-b", "master")
        (self.root / "harness.c").write_text(BASE, encoding="utf-8", newline="\n")
        self.git("add", "harness.c")
        self.git("commit", "-q", "-m", "base")
        self.git("switch", "-q", "-c", "change")

    def git(self, *args: str) -> None:
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
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)

    def run_check(self, name: str, text: str) -> tuple[int, str]:
        (self.root / name).write_text(text, encoding="utf-8", newline="\n")
        out = io.StringIO()
        cwd = Path.cwd()
        os.chdir(self.root)
        try:
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
                status = check.main(["--base-branch", "master", name])
        finally:
            os.chdir(cwd)
        return status, out.getvalue()

    def outgoing_entry(
        self,
        *,
        from_ref: str | None = None,
        to_ref: str | None = None,
        extra: tuple[str, ...] = (),
    ) -> tuple[int, str, str]:
        """Invoke the parsed push entry using the fixture's default comparison branch."""
        self.git("branch", "-f", "origin/master", "master")
        return run_push_entry(
            self.root, "flawfinder", from_ref=from_ref, to_ref=to_ref, extra=extra
        )

    def commit_outgoing(self, name: str, source: str) -> None:
        """Commit one source fixture without staging the copied hook implementation."""
        (self.root / name).write_text(source, encoding="utf-8", newline="\n")
        self.git("add", name)
        self.git("commit", "-q", "-m", "outgoing source")

    def test_push_hook_contract(self) -> None:
        """Only the push hook runs unconditionally without positional filenames."""
        assert_push_contract(self, "flawfinder")

    def test_installed_push_entry_absent_worktree_file(self) -> None:
        """A new-branch push catches its sole new source while master is checked out."""
        self.commit_outgoing("new.c", BASE)
        self.git("switch", "-q", "master")
        self.assertFalse((self.root / "new.c").exists())
        status, out, err = self.outgoing_entry(from_ref="master", to_ref="change")
        self.assertEqual(status, 1, err)
        self.assertIn("new.c:", out)

    def test_installed_push_entry_docs_followup(self) -> None:
        """A docs-only push still catches findings already on the remote feature tip."""
        self.commit_outgoing("new.c", BASE)
        self.git("branch", "previous-tip")
        self.commit_outgoing("README.md", "documentation only\n")
        self.git("switch", "-q", "master")
        status, out, err = self.outgoing_entry(from_ref="previous-tip", to_ref="change")
        self.assertEqual(status, 1, err)
        self.assertIn("new.c:", out)

    def test_outgoing_without_new_findings(self) -> None:
        """A safe outgoing addition exits 0 with no findings."""
        self.commit_outgoing("new.c", "int main(void) { return 0; }\n")
        self.git("switch", "-q", "master")
        status, out, err = self.outgoing_entry(to_ref="change")
        self.assertEqual(status, 0, err)
        self.assertEqual(out, "")

    def test_outgoing_deleted_only(self) -> None:
        """An outgoing deletion needs no source scan and exits 0."""
        self.git("rm", "harness.c")
        self.git("commit", "-q", "-m", "delete only")
        self.git("switch", "-q", "master")
        status, out, err = self.outgoing_entry(to_ref="change")
        self.assertEqual(status, 0, err)
        self.assertEqual(out, "")

    def test_outgoing_rejects_positional_files(self) -> None:
        """Outgoing selection combined with positional files is a usage error."""
        status, _, err = self.outgoing_entry(extra=("new.c",))
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
        self.commit_outgoing("new.c", BASE)
        (self.root / "new.c").write_text(
            "int main(void) { return 0; }\n", encoding="utf-8", newline="\n"
        )
        status, out, err = self.outgoing_entry()
        self.assertEqual(status, 1, err)
        self.assertIn("new.c:", out)

    def test_outgoing_rename_keeps_base_finding(self) -> None:
        """A Git-detected outgoing rename retains the base finding despite another checkout."""
        self.commit_outgoing("new.c", BASE)
        self.git("branch", "-f", "master", "HEAD")
        self.git("mv", "new.c", "renamed.c")
        self.git("commit", "-q", "-m", "rename")
        self.git("switch", "-q", "master")
        status, out, err = self.outgoing_entry(to_ref="change")
        self.assertEqual(status, 0, err)
        self.assertEqual(out, "")

    def test_push_scans_outgoing_blob(self) -> None:
        """A new outgoing fopen finding returns 1 even when the working copy lacks it."""
        (self.root / "harness.c").write_text(ADDED, encoding="utf-8", newline="\n")
        self.git("add", ".")
        self.git("commit", "-q", "-m", "outgoing")
        self.git("branch", "outgoing")
        self.git("switch", "-q", "master")
        with mock.patch.dict(os.environ, {"PRE_COMMIT_TO_REF": "outgoing"}):
            status, out = self.run_check("harness.c", BASE)
        self.assertEqual(status, 1)
        self.assertIn("harness.c:6: flawfinder fopen", out)

    def test_push_ignores_dirty_worktree(self) -> None:
        """An unchanged outgoing blob returns 0 despite a new finding in the working copy."""
        self.git("branch", "outgoing")
        with mock.patch.dict(os.environ, {"PRE_COMMIT_TO_REF": "outgoing"}):
            self.assertEqual(self.run_check("harness.c", ADDED), (0, ""))

    def test_literal_base_path(self) -> None:
        """A bracketed filename retains its base finding when source lines move and returns 0."""
        self.git("mv", "harness.c", "module[1].c")
        self.git("commit", "-q", "-m", "brackets")
        self.git("branch", "-f", "master", "HEAD")
        self.assertEqual(self.run_check("module[1].c", MOVED), (0, ""))

    def test_added_fopen_is_reported(self) -> None:
        status, out = self.run_check("harness.c", ADDED)
        self.assertEqual(status, 1)
        self.assertIn("harness.c:6: flawfinder fopen", out)
        self.assertNotIn("harness.c:5:", out, "the base finding was reported as new")

    def test_moved_base_finding_passes(self) -> None:
        self.assertEqual(self.run_check("harness.c", MOVED), (0, ""))

    def test_fdopen_on_descriptor_passes(self) -> None:
        self.assertEqual(self.run_check("harness.c", FDOPEN), (0, ""))

    def test_module_interface_suffix_is_scanned(self) -> None:
        status, out = self.run_check("fresh.ccm", BASE)
        self.assertEqual(status, 1)
        self.assertIn("fresh.ccm:5: flawfinder fopen", out)

    def test_renamed_file_keeps_its_base_findings(self) -> None:
        self.git("mv", "harness.c", "renamed.c")
        self.assertEqual(self.run_check("renamed.c", BASE), (0, ""))

    def test_renamed_file_with_an_added_finding_is_reported(self) -> None:
        self.git("mv", "harness.c", "renamed.c")
        status, out = self.run_check("renamed.c", ADDED)
        self.assertEqual(status, 1)
        self.assertIn("renamed.c:6: flawfinder fopen", out)
        self.assertNotIn("renamed.c:5:", out, "the base finding was reported as new")

    def test_new_file_finding_is_reported(self) -> None:
        status, out = self.run_check("fresh.c", BASE)
        self.assertEqual(status, 1)
        self.assertIn("fresh.c:5: flawfinder fopen", out)


class CheckFlawfinderHookEnvironment(unittest.TestCase):
    def test_fixture_ignores_outer_hook_refs(self) -> None:
        """An existing fixture test passes despite caller refs absent from its repository."""
        injected = {
            "PRE_COMMIT_TO_REF": "f" * 40,
            "PRE_COMMIT_FROM_REF": "e" * 40,
        }
        output = io.StringIO()
        with mock.patch.dict(os.environ, injected):
            suite = unittest.TestSuite([CheckFlawfinderNew("test_moved_base_finding_passes")])
            result = unittest.TextTestRunner(stream=output).run(suite)
            self.assertEqual(result.testsRun, 1)
            self.assertTrue(result.wasSuccessful(), output.getvalue())
            self.assertEqual({key: os.environ[key] for key in injected}, injected)


class CheckFlawfinderSuffixes(unittest.TestCase):
    TYPES = frozenset({"c", "c++", "cuda", "objective-c++"})  # the hook's types_or

    def test_suffixes_match_the_hook_selection(self) -> None:
        selected = {f".{ext}" for ext, tags in extensions.EXTENSIONS.items() if tags & self.TYPES}
        self.assertEqual(set(check.SUFFIXES), selected)


class CheckFlawfinderMissing(unittest.TestCase):
    def setUp(self) -> None:
        """Keep the in-process missing-tool check independent of caller hook variables."""
        isolate_pre_commit_environment(self)

    def test_missing_flawfinder_is_a_tool_error(self) -> None:
        with (
            mock.patch("shutil.which", return_value=None),
            contextlib.redirect_stderr(io.StringIO()) as err,
        ):
            self.assertEqual(check.main(["x.c"]), 2)
        self.assertIn("flawfinder is not on PATH", err.getvalue())


if __name__ == "__main__":
    unittest.main()
