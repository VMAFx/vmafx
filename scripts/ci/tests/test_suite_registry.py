#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Positive, negative and boundary controls for scripts/ci/suite_registry.py (ADR-1528).


Each case builds a throwaway Git repository holding a manifest, an aggregator
with a `required` list and a few test files, then runs the registry as a
subprocess of this interpreter, as CI does.
"""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.lib import git_env

ROOT = Path(__file__).resolve().parents[3]
REGISTRY = ROOT / "scripts" / "ci" / "suite_registry.py"
GIT = shutil.which("git")
TIMEOUT_SECONDS = 300

AGGREGATOR = """jobs:
  aggregator:
    steps:
      - with:
          script: |
            const required = [
              // a comment with an apostrophe isn't a name
              'Unit',
              'Tools',
            ];
"""


def manifest(**overrides: object) -> dict[str, object]:
    data: dict[str, object] = {
        "schema_version": 1,
        "name_patterns": ["test_*.py", "test-*.sh", "*_test.go"],
        "path_patterns": ["*/tests/*.rs"],
        "suites": [
            {"name": "unit", "paths": ["pkg/", "crate/"], "checks": ["Unit"], "not_local": "go"},
            {"name": "tools", "paths": ["tools/"], "checks": ["Tools"], "not_local": "tools"},
        ],
        "not_tests": [{"path": "tools/test-driver.sh", "reason": "a driver, not a test"}],
    }
    data.update(overrides)
    return data


PASSING_FILES = {
    "pkg/a_test.go": "package pkg\n",
    "crate/tests/smoke.rs": "#[test] fn ok() {}\n",
    "tools/test_ok.py": "def test_ok():\n    assert True\n",
    "tools/test-ok.sh": "#!/usr/bin/env bash\nexit 0\n",
    "tools/test-driver.sh": "#!/usr/bin/env bash\nexit 9\n",
    "tools/helper.py": "VALUE = 1\n",
}


class SuiteRegistryTests(unittest.TestCase):
    def setUp(self) -> None:
        if GIT is None:
            self.skipTest("git is required to build the fixture repositories")
        self.git_bin: str = GIT
        self._tmp = tempfile.TemporaryDirectory(prefix="suite-registry-")
        self.addCleanup(self._tmp.cleanup)
        self.repo = Path(self._tmp.name)
        self.env = git_env.drop()
        self.git("init", "-q")

    def git(self, *args: str) -> None:
        subprocess.run(  # noqa: S603 - fixed git argv in a temporary repository
            [self.git_bin, "-C", str(self.repo), *args],
            check=True,
            env=self.env,
            capture_output=True,
            timeout=TIMEOUT_SECONDS,
        )

    def build(self, files: dict[str, str], data: dict[str, object] | None = None) -> None:
        workflows = self.repo / ".github" / "workflows"
        workflows.mkdir(parents=True, exist_ok=True)
        (self.repo / ".github" / "test-suites.json").write_text(
            json.dumps(data if data is not None else manifest()), encoding="utf-8"
        )
        (workflows / "required-aggregator.yml").write_text(AGGREGATOR, encoding="utf-8")
        for relative, text in files.items():
            path = self.repo / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        self.git("add", "-A")

    def registry(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(  # noqa: S603 - this interpreter running the script under test
            [sys.executable, str(REGISTRY), "--root", str(self.repo), *args],
            check=False,
            env=self.env,
            capture_output=True,
            text=True,
            timeout=TIMEOUT_SECONDS,
        )

    def assert_finding(self, result: subprocess.CompletedProcess[str], text: str) -> None:
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn(text, result.stderr)

    # --- local-run fields (run_affected_suites.py) -----------------------------

    def test_suite_without_install_or_reason_is_a_usage_error(self) -> None:
        data = manifest()
        del data["suites"][0]["not_local"]  # type: ignore[index]
        self.build(PASSING_FILES, data)
        result = self.registry("check")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("exactly one of install and not_local", result.stderr)

    def test_install_naming_an_untracked_lock_fails(self) -> None:
        data = manifest()
        data["suites"][1].pop("not_local")  # type: ignore[index]
        data["suites"][1]["install"] = {"python": "3.14", "locks": ["tools/gone.txt"]}  # type: ignore[index]
        self.build(PASSING_FILES, data)
        self.assert_finding(self.registry("check"), "'tools/gone.txt' matches no tracked file")

    def test_venv_of_naming_no_suite_fails(self) -> None:
        data = manifest()
        data["suites"][1].pop("not_local")  # type: ignore[index]
        data["suites"][1]["install"] = {"python": "3.14", "venv_of": "nope"}  # type: ignore[index]
        self.build(PASSING_FILES, data)
        self.assert_finding(self.registry("check"), "venv_of names no suite: 'nope'")

    # --- check ---------------------------------------------------------------

    def test_wired_repository_passes(self) -> None:
        self.build(PASSING_FILES)
        result = self.registry("check")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("OK (4 test files in 2 suites)", result.stdout)

    def test_test_file_outside_every_suite_fails(self) -> None:
        self.build({**PASSING_FILES, "orphan/test_lost.py": "def test_x():\n    pass\n"})
        self.assert_finding(self.registry("check"), "orphan/test_lost.py: test file in no suite")

    def test_test_file_in_two_suites_fails(self) -> None:
        data = manifest()
        data["suites"][1]["paths"] = ["tools/", "pkg/"]  # type: ignore[index]
        self.build(PASSING_FILES, data)
        self.assert_finding(self.registry("check"), "pkg/a_test.go: test file in suites")

    def test_check_missing_from_aggregator_fails(self) -> None:
        data = manifest()
        data["suites"][1]["checks"] = ["Not Required"]  # type: ignore[index]
        self.build(PASSING_FILES, data)
        self.assert_finding(self.registry("check"), "check 'Not Required' is not in the required")

    def test_suite_path_without_tests_fails(self) -> None:
        data = manifest()
        data["suites"][0]["paths"] = ["pkg/", "crate/", "empty/"]  # type: ignore[index]
        self.build(PASSING_FILES, data)
        self.assert_finding(self.registry("check"), "path 'empty/' holds no test file")

    def test_stale_not_tests_entry_fails(self) -> None:
        files = dict(PASSING_FILES)
        del files["tools/test-driver.sh"]
        self.build(files)
        self.assert_finding(
            self.registry("check"), "'tools/test-driver.sh' matches no tracked file"
        )

    def test_untracked_test_file_is_not_counted(self) -> None:
        self.build(PASSING_FILES)
        (self.repo / "orphan").mkdir()
        (self.repo / "orphan" / "test_local.py").write_text("x = 1\n", encoding="utf-8")
        self.assertEqual(self.registry("check").returncode, 0)

    def test_duplicate_manifest_key_is_a_usage_error(self) -> None:
        self.build(PASSING_FILES)
        path = self.repo / ".github" / "test-suites.json"
        path.write_text('{"schema_version": 1, "schema_version": 1}', encoding="utf-8")
        result = self.registry("check")
        self.assertEqual(result.returncode, 2)
        self.assertIn("duplicate keys", result.stderr)

    # --- list / run ----------------------------------------------------------

    def test_list_names_the_suite_files_without_exclusions(self) -> None:
        self.build(PASSING_FILES)
        result = self.registry("list", "tools")
        self.assertEqual(result.stdout.split(), ["tools/test-ok.sh", "tools/test_ok.py"])

    def test_run_passes_when_every_test_passes(self) -> None:
        self.build(PASSING_FILES)
        result = self.registry("run", "tools")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("ok   tools/test-ok.sh", result.stdout)

    def test_run_fails_on_a_failing_python_test(self) -> None:
        self.build({**PASSING_FILES, "tools/test_bad.py": "def test_bad():\n    assert False\n"})
        result = self.registry("run", "tools")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("failed: pytest", result.stderr)

    def test_run_fails_on_a_failing_shell_test(self) -> None:
        self.build({**PASSING_FILES, "tools/test-bad.sh": "echo planted >&2\nexit 3\n"})
        result = self.registry("run", "tools")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("FAIL tools/test-bad.sh: exit 3", result.stdout)
        self.assertIn("planted", result.stdout)

    def test_run_refuses_a_suite_it_cannot_run(self) -> None:
        self.build(PASSING_FILES)
        result = self.registry("run", "unit")
        self.assertEqual(result.returncode, 2)
        self.assertIn("cannot run", result.stderr)

    def test_run_unknown_suite_is_a_usage_error(self) -> None:
        self.build(PASSING_FILES)
        result = self.registry("run", "nope")
        self.assertEqual(result.returncode, 2)
        self.assertIn("no suite named 'nope'", result.stderr)

    # --- one run per test (ADR-1568) ------------------------------------------

    def tooling_manifest(self) -> dict[str, object]:
        data = manifest()
        data["suites"] = [  # one file of tools/ leaves for a suite of its own
            {"name": "unit", "paths": ["pkg/", "crate/"], "checks": ["Unit"], "not_local": "go"},
            {"name": "tooling", "paths": ["tools/"], "checks": ["Tools"], "not_local": "t"},
            {"name": "pinned", "paths": ["tools/test_ok.py"], "checks": ["Unit"], "not_local": "p"},
        ]
        return data

    def test_most_specific_path_owns_the_file(self) -> None:
        self.build(PASSING_FILES, self.tooling_manifest())
        self.assertEqual(self.registry("check").returncode, 0)
        self.assertEqual(self.registry("list", "tooling").stdout.split(), ["tools/test-ok.sh"])
        self.assertEqual(self.registry("list", "pinned").stdout.split(), ["tools/test_ok.py"])

    def test_workflow_running_a_tooling_test_fails(self) -> None:
        workflow = "jobs:\n  x:\n    steps:\n      - run: bash tools/test-ok.sh\n"
        self.build({**PASSING_FILES, ".github/workflows/x.yml": workflow}, self.tooling_manifest())
        self.assert_finding(
            self.registry("check"), "x.yml: runs tools/test-ok.sh, which Tooling Tests"
        )

    def test_workflow_mentioning_a_tooling_test_in_a_comment_passes(self) -> None:
        workflow = "jobs:\n  x:\n    steps:\n      # tools/test-ok.sh runs in Tooling Tests\n"
        self.build({**PASSING_FILES, ".github/workflows/x.yml": workflow}, self.tooling_manifest())
        self.assertEqual(self.registry("check").returncode, 0)

    def test_workflow_running_a_test_of_another_suite_passes(self) -> None:
        workflow = "jobs:\n  x:\n    steps:\n      - run: python3 tools/test_ok.py\n"
        self.build({**PASSING_FILES, ".github/workflows/x.yml": workflow}, self.tooling_manifest())
        self.assertEqual(self.registry("check").returncode, 0)

    def test_precommit_skip_names_only_hooks_that_run_tooling_tests_alone(self) -> None:
        config = (
            "repos:\n"
            "  - repo: local\n"
            "    hooks:\n"
            "      - {id: tooling-only, entry: bash tools/test-ok.sh}\n"
            "      - {id: check, entry: python3 tools/helper.py}\n"
            "      - {id: mixed, entry: bash -c 'bash tools/test-ok.sh && python3 tools/helper.py'}\n"
            "      - {id: other-suite, entry: python3 tools/test_ok.py}\n"
            "      - {id: make-target, entry: make lint}\n"
        )
        self.build({**PASSING_FILES, ".pre-commit-config.yaml": config}, self.tooling_manifest())
        result = self.registry("precommit-skip")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "tooling-only")


class RepositoryRegistryTests(unittest.TestCase):
    def test_the_repository_is_fully_wired(self) -> None:
        result = subprocess.run(  # noqa: S603 - this interpreter running the script under test
            [sys.executable, str(REGISTRY), "check"],
            check=False,
            capture_output=True,
            text=True,
            timeout=TIMEOUT_SECONDS,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
