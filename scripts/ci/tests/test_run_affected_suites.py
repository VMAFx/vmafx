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
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.ci import run_affected_suites as runner  # noqa: E402
from scripts.ci import suite_registry as registry_lib  # noqa: E402
from scripts.lib import push_head  # noqa: E402
from scripts.lib.safe_subprocess import TextCommandResult  # noqa: E402
from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

SCRIPT = ROOT / "scripts" / "ci" / "run_affected_suites.py"
GIT = shutil.which("git")
TIMEOUT_SECONDS = 300
LOCK = "pkg/requirements-dev-lock.txt"
ROCM_SCRIPT = "scripts/ci/" + "install-rocm-" + "from-image.sh"
ROCM_NAME = Path(ROCM_SCRIPT).name


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


class FocusedSelectionTests(unittest.TestCase):
    def test_live_rocm_script_selects_exactly_its_two_tests(self) -> None:
        registry = registry_lib.load_registry(ROOT)
        tracked = registry_lib.tracked_files(ROOT)
        texts = {
            path: (ROOT / path).read_text(encoding="utf-8")
            for path in registry_lib.suite_files(registry, tracked, "tooling")
        }
        selected, unnamed = runner.focused_selection([ROCM_SCRIPT], tracked, texts)
        self.assertEqual(
            selected,
            [
                "scripts/ci/tests/test_install_rocm_from_image.py",
                "scripts/ci/tests/test_required_release_legs.py",
            ],
        )
        self.assertEqual(unnamed, [])

    def test_a_changed_test_selects_itself(self) -> None:
        test = "pkg/tests/test_changed.py"
        self.assertEqual(runner.focused_selection([test], [test], {test: ""}), ([test], []))

    def test_repo_relative_path_selects_with_ambiguous_basename(self) -> None:
        path = "pkg/meson.build"
        selected, unnamed = runner.focused_selection(
            [path], [path, "other/meson.build"], {"test.py": repr(path)}
        )
        self.assertEqual((selected, unnamed), (["test.py"], []))

    def test_unique_basename_selects(self) -> None:
        path = ROCM_SCRIPT
        selected, unnamed = runner.focused_selection(
            [path], [path], {"test.py": repr(Path(ROCM_SCRIPT).name)}
        )
        self.assertEqual((selected, unnamed), (["test.py"], []))

    def test_dotted_module_selects(self) -> None:
        path = "scripts/ci/check_dco.py"
        selected, unnamed = runner.focused_selection(
            [path], [path], {"test.py": "from scripts.ci.check_dco import main"}
        )
        self.assertEqual((selected, unnamed), (["test.py"], []))

    def test_unique_stem_in_import_selects(self) -> None:
        path = "scripts/ci/check_dco.py"
        for text in ("import check_dco", "from scripts.ci import (\n    check_dco,\n)"):
            with self.subTest(text=text):
                self.assertEqual(
                    runner.focused_selection([path], [path], {"test.py": text}),
                    (["test.py"], []),
                )

    def test_nonunique_basename_selects_nothing(self) -> None:
        for name in ("__init__.py", "meson.build"):
            with self.subTest(name=name):
                path = f"pkg/{name}"
                self.assertEqual(
                    runner.focused_selection(
                        [path], [path, f"other/{name}"], {"test.py": repr(name)}
                    ),
                    ([], [path]),
                )

    def test_longer_path_token_does_not_select(self) -> None:
        path = ROCM_SCRIPT
        for text in (
            "'xinstall-rocm-from-image.sh.bak'",
            "'install-rocm-from-image.sh.bak'",
            "'install-rocm-from-image.sh-old'",
            "'scripts/ci/install-rocm-from-image.sh/x'",
        ):
            with self.subTest(text=text):
                self.assertEqual(
                    runner.focused_selection([path], [path], {"test.py": text}), ([], [path])
                )

    def test_path_after_a_root_prefix_selects(self) -> None:
        path = ROCM_SCRIPT
        tracked = [path, "other/" + ROCM_NAME]
        for text in (
            'f"{ROOT}/' + path + '"',
            "'./" + path + "'",
            "# runs " + path + ".",
        ):
            with self.subTest(text=text):
                self.assertEqual(
                    runner.focused_selection([path], tracked, {"test.py": text}),
                    (["test.py"], []),
                )

    def test_unique_basename_after_a_directory_selects(self) -> None:
        path = ROCM_SCRIPT
        text = "HERE / '../" + ROCM_NAME + "'"
        self.assertEqual(
            runner.focused_selection([path], [path], {"test.py": text}), (["test.py"], [])
        )

    def test_dotted_module_attribute_selects(self) -> None:
        path = "scripts/ci/check_dco.py"
        text = "mock.patch('scripts.ci.check_dco.run')"
        self.assertEqual(
            runner.focused_selection([path], [path], {"test.py": text}), (["test.py"], [])
        )

    def test_stem_in_prose_or_alias_does_not_select(self) -> None:
        path = "scripts/ci/check_dco.py"
        for text in ("# check_dco", "import other as check_dco", "# invalid Python\n!check_dco"):
            with self.subTest(text=text):
                self.assertEqual(
                    runner.focused_selection([path], [path], {"test.py": text}), ([], [path])
                )

    def test_nonunique_stem_in_import_does_not_select(self) -> None:
        path = "pkg/common.py"
        self.assertEqual(
            runner.focused_selection(
                [path], [path, "other/common.py"], {"test.py": "from pkg import common"}
            ),
            ([], [path]),
        )

    def test_longer_module_does_not_select(self) -> None:
        path = "pkg/mod.py"
        self.assertEqual(
            runner.focused_selection([path], [path], {"test.py": "# pkg.mod_extra"}),
            ([], [path]),
        )


class PushHeadTests(unittest.TestCase):
    def test_equal_outgoing_ref_passes_for_both_framework_variables(self) -> None:
        for variable in ("PRE_COMMIT_TO_REF", "PRE_COMMIT_LOCAL_BRANCH"):
            with (
                self.subTest(variable=variable),
                patch.object(os, "environ", {variable: "pushed", "GIT_DIR": "/unrelated"}),
                patch.object(push_head, "run_command") as command,
            ):
                command.return_value.returncode = 0
                command.return_value.stdout = "same\n"
                push_head.outgoing_head("same", ROOT)
                self.assertEqual(command.call_args.kwargs["env"], {variable: "pushed"})
                self.assertEqual(command.call_args.args[0][-1], "pushed^{commit}")

    def test_differing_ref_raises(self) -> None:
        with (
            patch.object(os, "environ", {"PRE_COMMIT_TO_REF": "pushed"}),
            patch.object(push_head, "run_command") as command,
        ):
            command.return_value.returncode = 0
            command.return_value.stdout = "other\n"
            with self.assertRaisesRegex(RuntimeError, "outgoing ref differs from HEAD"):
                push_head.outgoing_head("head", ROOT)

    def test_unset_outgoing_ref_passes_without_git(self) -> None:
        with (
            patch.object(os, "environ", {}),
            patch.object(push_head, "run_command") as command,
        ):
            push_head.outgoing_head("head", ROOT)
            command.assert_not_called()

    def test_missing_outgoing_ref_fails_closed(self) -> None:
        with (
            patch.object(os, "environ", {"PRE_COMMIT_TO_REF": "missing"}),
            patch.object(push_head, "run_command") as command,
        ):
            command.return_value.returncode = 128
            command.return_value.stderr = "missing"
            with self.assertRaisesRegex(RuntimeError, "cannot resolve outgoing ref"):
                push_head.outgoing_head("head", ROOT)


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
        self.env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
        self.git("init", "-q")
        self.parent: str | None = None

    def git(self, *args: str, input_data: str | None = None) -> str:
        result = run_command(
            [str(GIT), "-C", str(self.repo), *args],
            allowed_executables=(str(GIT),),
            input_data=input_data,
            text=True,
            check=True,
            env=self.env,
            capture_output=True,
            timeout_seconds=TIMEOUT_SECONDS,
        )

        return result.stdout.strip()

    def commit(self) -> None:
        """Write fixture history without invoking a credential-backed signing tool."""
        tree = self.git("write-tree")
        parent = f"parent {self.parent}\n" if self.parent else ""
        content = (
            f"tree {tree}\n{parent}"
            "author Fixture <fixture@example.invalid> 1 +0000\n"
            "committer Fixture <fixture@example.invalid> 1 +0000\n\nfixture\n"
        )
        self.parent = self.git("hash-object", "-t", "commit", "-w", "--stdin", input_data=content)
        self.git("update-ref", "HEAD", self.parent)

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

    def run_script(self, *args: str) -> TextCommandResult:
        return run_command(
            [
                sys.executable,
                str(SCRIPT),
                "--root",
                str(self.repo),
                "--python-exe",
                sys.executable,
                *args,
            ],
            allowed_executables=(sys.executable,),
            check=False,
            env=self.env,
            capture_output=True,
            text=True,
            timeout_seconds=TIMEOUT_SECONDS,
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
        self.commit()
        (self.repo / "pkg" / "tests" / "test_ok.py").write_text("def test_ok():\n    assert 1\n")
        self.git("add", "-A")
        self.commit()
        result = self.run_script("--list", "--base", "HEAD~1", "--head", "HEAD")
        self.assertIn("pkg: 1 changed file(s), e.g. pkg/tests/test_ok.py", result.stdout)

    def test_focused_unnamed_file_is_not_run_or_counted_passed(self) -> None:
        self.build({"pkg/tests/test_bad.py": "def test_bad():\n    assert False\n"})
        result = self.run_script("--focused", "--files", LOCK)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f"NOT RUN {LOCK}: no test names this file", result.stdout)
        self.assertIn("pkg: NOT RUN (no test names a changed file)", result.stdout)
        self.assertNotIn("passed", result.stdout)
        strict = self.run_script("--strict", "--focused", "--files", LOCK)
        self.assertEqual(strict.returncode, 1, strict.stdout + strict.stderr)

    def test_focused_changed_test_runs_only_itself(self) -> None:
        self.build(
            {
                "pkg/tests/test_ok.py": "def test_ok():\n    assert True\n",
                "pkg/tests/test_bad.py": "def test_bad():\n    assert False\n",
            }
        )
        result = self.run_script("--focused", "--files", "pkg/tests/test_ok.py")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("selected 1 test file(s)", result.stdout)
        self.assertIn("pkg: passed: 1 passed", result.stdout)

    def test_focused_named_and_unnamed_files_report_separately(self) -> None:
        self.build({"pkg/tests/test_ok.py": "def test_ok():\n    assert True\n"})
        result = self.run_script("--focused", "--files", f"{LOCK},pkg/tests/test_ok.py")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f"NOT RUN {LOCK}: no test names this file", result.stdout)
        self.assertIn("pkg: passed: 1 passed", result.stdout)

    def test_merge_base_conflicting_modes_are_usage_errors(self) -> None:
        for args in (("--files", "x"), ("--base", "x", "--head", "y"), ("--head", "x")):
            with self.subTest(args=args):
                self.assertEqual(self.run_script("--merge-base", "HEAD", *args).returncode, 2)

    def test_merge_base_uses_head_and_checks_outgoing_ref(self) -> None:
        self.build({"pkg/tests/test_ok.py": "def test_ok():\n    assert True\n"})
        self.commit()
        path = self.repo / "pkg/tests/test_ok.py"
        path.write_text("def test_ok():\n    assert 1\n", encoding="utf-8")
        self.git("add", "-A")
        self.commit()
        self.env["PRE_COMMIT_TO_REF"] = "HEAD"
        good = self.run_script("--focused", "--merge-base", "HEAD~1")
        self.assertEqual(good.returncode, 0, good.stdout + good.stderr)
        self.assertIn("pkg: passed: 1 passed", good.stdout)
        self.env["PRE_COMMIT_TO_REF"] = "HEAD~1"
        bad = self.run_script("--focused", "--merge-base", "HEAD~1")
        self.assertEqual(bad.returncode, 2, bad.stdout + bad.stderr)
        self.assertIn("outgoing ref differs from HEAD", bad.stderr)

    def test_base_without_head_is_a_usage_error(self) -> None:
        self.build({})
        self.assertEqual(self.run_script("--base", "HEAD").returncode, 2)


if __name__ == "__main__":
    unittest.main()
