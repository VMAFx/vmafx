#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Positive, negative and boundary controls for scripts/ci/run_affected_suites.py.

Mapping and cache-key cases call the module; run cases build a throwaway Git
repository with a manifest and run the script as a subprocess, with this
interpreter standing in for a cached venv (``--python-exe``), so no network or
installer is involved.
"""

from __future__ import annotations

import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from scripts.ci import run_affected_suites as runner  # noqa: E402
from scripts.ci import suite_registry as registry_lib  # noqa: E402
from scripts.lib import git_env  # noqa: E402

SCRIPT = ROOT / "scripts" / "ci" / "run_affected_suites.py"
GIT = shutil.which("git")
TIMEOUT_SECONDS = 300
LOCK = "pkg/requirements-dev-lock.txt"


def manifest() -> dict[str, object]:
    return {
        "schema_version": 1,
        "name_patterns": ["test_*.py"],
        "suites": [
            {
                "name": "ai",
                "paths": ["ai/tests/"],
                "checks": ["Tiny AI"],
                "source_paths": ["ai/src/", "model/"],
                "install": {"python": "3.14", "locks": ["ai/lock.txt"], "editable": ["ai"]},
            },
            {
                "name": "pkg",
                "paths": ["pkg/tests/"],
                "checks": ["Pkg"],
                "install": {"python": "3.14", "locks": [LOCK]},
            },
            {
                "name": "train",
                "paths": ["train/test_train.py"],
                "checks": ["Tiny AI"],
                "install": {"python": "3.14", "venv_of": "ai"},
            },
            {"name": "go", "paths": ["cmd/"], "checks": ["Go"], "not_local": "needs the Go build"},
        ],
        "not_tests": [],
    }


class MappingTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="affected-map-")
        self.addCleanup(self._tmp.cleanup)
        root = Path(self._tmp.name)
        (root / ".github").mkdir()
        (root / ".github" / "test-suites.json").write_text(json.dumps(manifest()), encoding="utf-8")
        self.registry = registry_lib.load_registry(root)

    def affected(self, *files: str) -> dict[str, list[str]]:
        return runner.affected_suites(self.registry, files)

    def test_package_source_selects_its_suite(self) -> None:
        self.assertEqual(sorted(self.affected("ai/src/vmaf_train/x.py")), ["ai", "train"])

    def test_a_test_file_selects_its_suite_only(self) -> None:
        self.assertEqual(sorted(self.affected("pkg/tests/test_a.py")), ["pkg"])

    def test_docs_only_change_selects_nothing(self) -> None:
        self.assertEqual(self.affected("docs/usage/cli.md", "README.md", "ai-notes.md"), {})

    def test_a_lock_file_selects_the_suites_that_run_in_its_venv(self) -> None:
        self.assertEqual(sorted(self.affected("ai/lock.txt")), ["ai", "train"])
        self.assertEqual(sorted(self.affected(LOCK)), ["pkg"])

    def test_a_sibling_directory_with_the_same_prefix_is_not_selected(self) -> None:
        self.assertEqual(self.affected("ai-extra/src/x.py", "model2/x.bin"), {})

    def test_a_file_path_suite_is_selected_by_that_file(self) -> None:
        self.assertEqual(sorted(self.affected("train/test_train.py")), ["train"])

    def test_a_suite_that_cannot_run_locally_is_still_reported(self) -> None:
        self.assertEqual(sorted(self.affected("cmd/x/main.go")), ["go"])

    def test_file_list_accepts_commas_spaces_and_newlines(self) -> None:
        self.assertEqual(
            runner.parse_file_list("a.py,b.py c.py\nd.py,,"), ["a.py", "b.py", "c.py", "d.py"]
        )


class VenvKeyTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="affected-key-")
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        (self.root / ".github").mkdir()
        (self.root / ".github" / "test-suites.json").write_text(
            json.dumps(manifest()), encoding="utf-8"
        )
        (self.root / "ai").mkdir()
        (self.root / "ai" / "lock.txt").write_text("torch==1 --hash=sha256:aa\n", encoding="utf-8")
        self.registry = registry_lib.load_registry(self.root)
        self.ai = next(s for s in self.registry.suites if s.name == "ai")

    def test_key_is_stable_for_unchanged_locks(self) -> None:
        self.assertEqual(runner.venv_key(self.root, self.ai), runner.venv_key(self.root, self.ai))

    def test_key_changes_when_a_lock_changes(self) -> None:
        before = runner.venv_key(self.root, self.ai)
        (self.root / "ai" / "lock.txt").write_text("torch==2 --hash=sha256:bb\n", encoding="utf-8")
        self.assertNotEqual(before, runner.venv_key(self.root, self.ai))

    def test_key_is_a_sha256_of_the_lock_bytes(self) -> None:
        self.assertRegex(runner.venv_key(self.root, self.ai), r"^[0-9a-f]{64}$")
        expected = hashlib.sha256()
        expected.update(
            f"{runner.VENV_FORMAT}\x003.14\x00ai/lock.txt\x00".encode()
            + (self.root / "ai" / "lock.txt").read_bytes()
        )
        self.assertEqual(runner.venv_key(self.root, self.ai), expected.hexdigest())

    def test_a_missing_lock_is_an_error(self) -> None:
        (self.root / "ai" / "lock.txt").unlink()
        with self.assertRaises(runner.RunnerError):
            runner.venv_key(self.root, self.ai)

    def test_a_suite_sharing_a_venv_resolves_to_its_owner(self) -> None:
        train = next(s for s in self.registry.suites if s.name == "train")
        self.assertEqual(runner.venv_owner(self.registry, train).name, "ai")


class VenvLockTests(unittest.TestCase):
    def test_a_held_lock_times_out_the_second_taker(self) -> None:
        with tempfile.TemporaryDirectory(prefix="affected-lock-") as tmp:
            path = Path(tmp) / "ai.lock"
            with runner.VenvLock(path, 5):
                with self.assertRaises(runner.RunnerError), runner.VenvLock(path, 0.5):
                    self.fail("the second taker must not get the lock")

    def test_a_released_lock_can_be_taken_again(self) -> None:
        with tempfile.TemporaryDirectory(prefix="affected-lock-") as tmp:
            path = Path(tmp) / "ai.lock"
            with runner.VenvLock(path, 5):
                pass
            with runner.VenvLock(path, 0.5):
                pass


class SkipPolicyTests(unittest.TestCase):
    def suite(self, fail_on_skip: str | None) -> registry_lib.Suite:
        return registry_lib.Suite("s", ("t/",), ("c",), fail_on_skip=fail_on_skip)

    def test_missing_package_skip_is_a_violation(self) -> None:
        skips = [("t::a", "could not import 'torch': No module named 'torch'")]
        self.assertEqual(len(runner.skip_violations(self.suite(None), skips)), 1)

    def test_declared_input_skip_is_a_violation(self) -> None:
        skips = [("t::a", "vmaf binary not found")]
        self.assertEqual(runner.skip_violations(self.suite(None), skips), [])
        self.assertEqual(len(runner.skip_violations(self.suite("vmaf binary"), skips)), 1)

    def test_an_unrelated_skip_is_not_a_violation(self) -> None:
        self.assertEqual(runner.skip_violations(self.suite(None), [("t::a", "needs a GPU")]), [])


@unittest.skipIf(GIT is None, "git is required to build the fixture repositories")
class RunTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="affected-run-")
        self.addCleanup(self._tmp.cleanup)
        self.repo = Path(self._tmp.name)
        self.env = git_env.drop()
        self.git("init", "-q")

    def git(self, *args: str) -> None:
        subprocess.run(  # noqa: S603 - fixed git argv in a temporary repository
            [str(GIT), "-C", str(self.repo), *args],
            check=True,
            env=self.env,
            capture_output=True,
            timeout=TIMEOUT_SECONDS,
        )

    def build(self, tests: dict[str, str]) -> None:
        files = {
            ".github/test-suites.json": json.dumps(manifest()),
            LOCK: "# lock\n",
            "ai/lock.txt": "# lock\n",
            "ai/src/mod.py": "VALUE = 1\n",
            **tests,
        }
        for relative, text in files.items():
            path = self.repo / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        self.git("add", "-A")

    def run_script(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(  # noqa: S603 - this interpreter running the script under test
            [
                sys.executable,
                str(SCRIPT),
                "--root",
                str(self.repo),
                "--python-exe",
                sys.executable,
                *args,
            ],
            check=False,
            env=self.env,
            capture_output=True,
            text=True,
            timeout=TIMEOUT_SECONDS,
        )

    def test_docs_only_change_runs_nothing_and_passes(self) -> None:
        self.build({"pkg/tests/test_ok.py": "def test_ok():\n    assert True\n"})
        result = self.run_script("--files", "docs/x.md")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("no suite is affected", result.stdout)

    def test_list_prints_the_affected_suites_without_running(self) -> None:
        self.build({"pkg/tests/test_bad.py": "def test_bad():\n    assert False\n"})
        result = self.run_script("--list", "--files", "pkg/tests/test_bad.py")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("pkg: 1 changed file(s)", result.stdout)
        self.assertNotIn("passed", result.stdout)

    def test_passing_suite_exits_zero_with_one_line(self) -> None:
        self.build({"pkg/tests/test_ok.py": "def test_ok():\n    assert True\n"})
        result = self.run_script("--files", "pkg/tests/test_ok.py")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertRegex(result.stdout, r"pkg: passed: 1 passed, 0 failed, 0 skipped in [\d.]+s")

    def test_planted_failing_test_fails_the_run(self) -> None:
        self.build({"pkg/tests/test_bad.py": "def test_bad():\n    assert False\n"})
        result = self.run_script("--files", "pkg/tests/test_bad.py")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("pkg: FAILED: 0 passed, 1 failed", result.stdout)
        self.assertIn("test_bad", result.stderr)

    def test_missing_dependency_skip_fails_the_run(self) -> None:
        body = "import pytest\n\ndef test_dep():\n    pytest.importorskip('no_such_pkg_vmafx')\n"
        self.build({"pkg/tests/test_dep.py": body})
        result = self.run_script("--files", "pkg/tests/test_dep.py")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("skipped: could not import", result.stderr)

    def test_an_ordinary_skip_passes(self) -> None:
        body = "import pytest\n\ndef test_gpu():\n    pytest.skip('needs a GPU')\n"
        self.build({"pkg/tests/test_gpu.py": body})
        result = self.run_script("--files", "pkg/tests/test_gpu.py")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("1 skipped", result.stdout)

    def test_time_cap_fails_a_slow_suite(self) -> None:
        body = "import time\n\ndef test_slow():\n    time.sleep(30)\n"
        self.build({"pkg/tests/test_slow.py": body})
        result = self.run_script("--time-cap", "2", "--files", "pkg/tests/test_slow.py")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("time cap", result.stderr)

    def test_suite_that_cannot_run_locally_is_reported_not_passed(self) -> None:
        self.build({"pkg/tests/test_ok.py": "def test_ok():\n    assert True\n"})
        lenient = self.run_script("--files", "cmd/x.go")
        self.assertEqual(lenient.returncode, 0, lenient.stderr)
        self.assertIn("go: NOT RUN", lenient.stdout)
        strict = self.run_script("--strict", "--files", "cmd/x.go")
        self.assertEqual(strict.returncode, 1)

    def test_base_and_head_select_the_files_that_differ(self) -> None:
        self.build({"pkg/tests/test_ok.py": "def test_ok():\n    assert True\n"})
        self.git("-c", "user.name=t", "-c", "user.email=t@example.invalid", "commit", "-qm", "base")
        (self.repo / "pkg" / "tests" / "test_ok.py").write_text("def test_ok():\n    assert 1\n")
        self.git("add", "-A")
        self.git("-c", "user.name=t", "-c", "user.email=t@example.invalid", "commit", "-qm", "head")
        result = self.run_script("--list", "--base", "HEAD~1", "--head", "HEAD")
        self.assertIn("pkg: 1 changed file(s), e.g. pkg/tests/test_ok.py", result.stdout)

    def test_base_without_head_is_a_usage_error(self) -> None:
        self.build({})
        self.assertEqual(self.run_script("--base", "HEAD").returncode, 2)


if __name__ == "__main__":
    unittest.main()
