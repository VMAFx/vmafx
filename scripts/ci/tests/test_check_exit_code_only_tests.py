#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Unit and git-diff contract tests for scripts/ci/check_exit_code_only_tests.py."""

from __future__ import annotations

import contextlib
import functools
import io
import os
import shutil
import sys
import tempfile
import unittest
from collections.abc import Callable
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from scripts.ci import check_exit_code_only_tests as checker  # noqa: E402
from scripts.ci.tests.scanner_test_support import isolate_pre_commit_environment  # noqa: E402
from scripts.lib import new_scanner_findings  # noqa: E402
from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

GIT = shutil.which("git")
GIT_TIMEOUT_S = 30.0
REL_TEST_FILE = "scripts/ci/tests/test_sample.py"
UNRELATED_OVER_BOUND_COUNT = 2_100


class ExitCodeOnlyFixture(unittest.TestCase):
    """Throwaway git repository fixture for exit-code-only tests."""

    def setUp(self) -> None:
        if GIT is None:
            self.skipTest("git executable is required for throwaway repository fixtures")
        self.git_bin: str = GIT
        isolate_pre_commit_environment(self)
        self._tmp = tempfile.TemporaryDirectory(prefix="exit-code-only-test-")
        self.addCleanup(self._tmp.cleanup)
        self.repo = Path(self._tmp.name)
        (self.repo / "scripts" / "ci" / "tests").mkdir(parents=True, exist_ok=True)
        self.env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
        self.env.update(
            GIT_CONFIG_GLOBAL=os.devnull,
            GIT_CONFIG_NOSYSTEM="1",
            GIT_AUTHOR_NAME="Test",
            GIT_AUTHOR_EMAIL="test@example.invalid",
            GIT_COMMITTER_NAME="Test",
            GIT_COMMITTER_EMAIL="test@example.invalid",
        )
        self._git("init", "-q")

    def _git(self, *args: str) -> str:
        result = run_command(
            [
                self.git_bin,
                "-C",
                str(self.repo),
                "-c",
                "commit.gpgsign=false",
                "-c",
                "core.hooksPath=/dev/null",
                *args,
            ],
            allowed_executables=(self.git_bin,),
            env=self.env,
            capture_output=True,
            text=True,
            check=True,
            timeout_seconds=GIT_TIMEOUT_S,
        )
        return result.stdout.strip()

    def _commit_file(self, rel_path: str, content: str, message: str = "test commit") -> Path:
        target = self.repo / rel_path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content, encoding="utf-8")
        self._git("add", "--", rel_path)
        self._git("commit", "-q", "-m", message)
        return target

    def _run_checker(self, *args: str) -> tuple[int, str, str]:
        out = io.StringIO()
        err = io.StringIO()

        # Git env builders strip GIT_*; inject isolation at the subprocess boundary.
        def isolated_run(
            original: Callable[..., object], *args: object, **kwargs: object
        ) -> object:
            env = kwargs["env"]
            assert isinstance(env, dict)
            env.update(GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1")
            return original(*args, **kwargs)

        with contextlib.ExitStack() as stack:
            stack.enter_context(contextlib.redirect_stdout(out))
            stack.enter_context(contextlib.redirect_stderr(err))
            for module in (checker, new_scanner_findings):
                original = module.run_command
                stack.enter_context(
                    mock.patch.object(
                        module, "run_command", functools.partial(isolated_run, original)
                    )
                )
            rc = checker.main(["--root", str(self.repo), *args])
        return rc, out.getvalue(), err.getvalue()


class ExitCodeOnlyTestsCheckTest(ExitCodeOnlyFixture):
    """Git configuration and fixture environment tests."""

    def test_planted_new_weak_test_fails_with_file_line_and_function(self) -> None:
        self._commit_file(REL_TEST_FILE, "import subprocess\n", "base")
        self._commit_file(
            REL_TEST_FILE,
            (
                "import subprocess\n"
                "import unittest\n\n"
                "class SampleTest(unittest.TestCase):\n"
                "    def test_weak_returncode_only(self) -> None:\n"
                "        res = subprocess.run(['true'], check=False)\n"
                "        self.assertEqual(res.returncode, 0)\n"
            ),
            "add weak test",
        )
        rc, _out, err = self._run_checker("--changed-against", "HEAD~1")
        self.assertEqual(rc, 1)
        self.assertIn(f"{REL_TEST_FILE}:5: test_weak_returncode_only:", err)
        self.assertIn("asserts only exit code 0", err)

    def test_color_always_git_config_still_reports_changed_findings(self) -> None:
        self._git("config", "color.ui", "always")
        self._git("config", "color.diff", "always")
        base = (
            "import subprocess\n\n"
            "def test_untouched_under_color() -> None:\n"
            "    r = subprocess.run(['true'], check=False)\n"
            "    assert r.returncode == 0\n\n"
            "def test_modified_under_color() -> None:\n"
            "    r = subprocess.run(['true'], check=False)\n"
            "    assert r.returncode == 0\n"
        )
        self._commit_file(REL_TEST_FILE, base, "base under color.ui=always")
        changed = (
            base.replace(
                "def test_modified_under_color() -> None:\n"
                "    r = subprocess.run(['true'], check=False)\n",
                "def test_modified_under_color() -> None:\n"
                "    r = subprocess.run(['true', '-v'], check=False)\n",
            )
            + "\ndef test_weak_under_color() -> None:\n"
            + "    r = subprocess.run(['true'], check=False)\n"
            + "    assert r.returncode == 0\n\n"
            + "def test_strong_under_color() -> None:\n"
            + "    r = subprocess.run(['true'], check=False)\n"
            + "    assert r.returncode == 0\n"
            + "    assert r.stdout == 'ok'\n"
        )
        self._commit_file(REL_TEST_FILE, changed, "changes under color.ui=always")
        rc, _out, err = self._run_checker("--changed-against", "HEAD~1")
        self.assertEqual(rc, 1)
        self.assertIn("test_modified_under_color: asserts only exit code 0", err)
        self.assertIn("test_weak_under_color: asserts only exit code 0", err)
        self.assertNotIn("test_untouched_under_color", err)
        self.assertNotIn("test_strong_under_color", err)

    def test_noprefix_mnemonic_ext_diff_and_renames_config_neutralised(self) -> None:
        self._git("config", "diff.noprefix", "true")
        self._git("config", "diff.mnemonicPrefix", "true")
        self._git("config", "diff.external", "/bin/false")
        self._git("config", "diff.renames", "false")
        other_old = "scripts/ci/tests/test_other_old.py"
        other_new = "scripts/ci/tests/test_other_new.py"
        weak_body = (
            "import subprocess\n\n"
            "def test_untouched_in_renamed() -> None:\n"
            "    r = subprocess.run(['true'], check=False)\n"
            "    assert r.returncode == 0\n"
        )
        self._commit_file(other_old, weak_body, "base rename source")
        self._commit_file(REL_TEST_FILE, "import subprocess\n", "base target")
        self._git("mv", other_old, other_new)
        self._commit_file(
            REL_TEST_FILE,
            (
                "import subprocess\n\n"
                "def test_prefix_weak() -> None:\n"
                "    r = subprocess.run(['true'], check=False)\n"
                "    assert r.returncode == 0\n\n"
                "def test_prefix_strong() -> None:\n"
                "    r = subprocess.run(['true'], check=False)\n"
                "    assert r.returncode == 0\n"
                "    assert r.stdout == 'ok'\n"
            ),
            "rename and edit under adversarial diff config",
        )
        rc, _out, err = self._run_checker("--changed-against", "HEAD~1")
        self.assertEqual(rc, 1)
        self.assertIn("test_prefix_weak: asserts only exit code 0", err)
        self.assertNotIn("test_prefix_strong", err)
        self.assertNotIn("test_untouched_in_renamed", err)

    def test_umlaut_filename_with_and_without_quotepath_fails(self) -> None:
        self._commit_file(REL_TEST_FILE, "# init\n", "init")
        umlaut_rel = "scripts/ci/tests/test_ümlaut.py"
        weak_code = "def test_umlaut_weak() -> None:\n    rc = run_helper()\n    assert rc == 0\n"
        for quotepath in ("true", "false"):
            self._git("config", "core.quotepath", quotepath)
            (self.repo / umlaut_rel).write_text(weak_code, encoding="utf-8")
            rc_u, _out_u, err_u = self._run_checker("--changed-against", "HEAD")
            self.assertEqual(rc_u, 1)
            self.assertIn(f"{umlaut_rel}:1: test_umlaut_weak: asserts only exit code 0", err_u)
            (self.repo / umlaut_rel).unlink()
        target = self._commit_file(umlaut_rel, "# base\n", "base umlaut")
        target.write_text(weak_code, encoding="utf-8")
        for quotepath in ("true", "false"):
            self._git("config", "core.quotepath", quotepath)
            rc, _out, err = self._run_checker("--changed-against", "HEAD")
            self.assertEqual(rc, 1)
            self.assertIn(f"{umlaut_rel}:1: test_umlaut_weak: asserts only exit code 0", err)

    def test_inter_hunk_config_does_not_touch_old_weak_test(self) -> None:
        base = (
            "# before\n\n"
            + "def test_old():\n    rc = run_checker()\n    assert rc == 0\n"
            + "\ndef helper():\n    pass\n# after\n"
        )
        target = self._commit_file(REL_TEST_FILE, base)
        self._git("config", "diff.interHunkContext", "20")
        target.write_text(
            base.replace("# before", "# changed before").replace("# after", "# changed after"),
            encoding="utf-8",
        )
        rc, out, err = self._run_checker("--changed-against", "HEAD")
        self.assertEqual(rc, 0, err)
        self.assertIn("0 exit-code-only test findings", out)


class ExitCodeOnlyHookEnvironmentTest(unittest.TestCase):
    def test_fixture_ignores_outer_hook_refs(self) -> None:
        """A changed-mode fixture test passes despite push refs absent from its repository."""
        injected = {
            "PRE_COMMIT_TO_REF": "f" * 40,
            "PRE_COMMIT_FROM_REF": "e" * 40,
        }
        output = io.StringIO()
        with mock.patch.dict(os.environ, injected):
            case = ExitCodeOnlyTestsCheckTest(
                "test_planted_new_weak_test_fails_with_file_line_and_function"
            )
            result = unittest.TextTestRunner(stream=output).run(unittest.TestSuite([case]))
            self.assertEqual(result.testsRun, 1)
            self.assertTrue(result.wasSuccessful(), output.getvalue())
            self.assertEqual({key: os.environ[key] for key in injected}, injected)


if __name__ == "__main__":
    unittest.main()
