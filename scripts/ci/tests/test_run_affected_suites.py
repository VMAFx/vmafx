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
    def test_package_initializer_selects_imports_of_package(self) -> None:
        """Package imports cover their initializer despite shared initializer basenames."""
        path = "src/widget/__init__.py"
        tracked = [path, "other/__init__.py"]
        for text in ("import widget", "from widget import value", "import src.widget"):
            with self.subTest(text=text):
                self.assertEqual(
                    runner.focused_selection([path], tracked, {"test.py": text}),
                    (["test.py"], []),
                )

    def test_ambiguous_package_leaf_requires_qualified_reference(self) -> None:
        """Duplicate package leaves cannot claim a short import."""
        path = "widget/__init__.py"
        tracked = [path, "other/widget/__init__.py"]
        for text in ("import widget", "import widget_extra", "# widget"):
            with self.subTest(text=text):
                self.assertEqual(
                    runner.focused_selection([path], tracked, {"test.py": text}), ([], [path])
                )
        self.assertEqual(
            runner.focused_selection([path], tracked, {"test.py": repr(path)}),
            (["test.py"], []),
        )

    def test_package_import_boundaries_and_qualified_duplicate(self) -> None:
        """Qualified packages distinguish duplicate leaves and reject longer names."""
        path = "src/widget/__init__.py"
        tracked = [path, "other/widget/__init__.py"]
        self.assertEqual(
            runner.focused_selection([path], tracked, {"test.py": "import src.widget"}),
            (["test.py"], []),
        )
        for text in ("import widget_extra", "import src.widget_extra", "import other.widget"):
            with self.subTest(text=text):
                self.assertEqual(
                    runner.focused_selection([path], tracked, {"test.py": text}), ([], [path])
                )

    def test_vmaf_named_package_outside_alias_roots_keeps_ambiguity(self) -> None:
        """The compatibility alias cannot exempt an unrelated package with the same name."""
        path = "vmaf/__init__.py"
        self.assertEqual(
            runner.focused_selection(
                [path], [path, "other/vmaf/__init__.py"], {"test.py": "import vmaf"}
            ),
            ([], [path]),
        )

    def test_compat_package_alias_selects_vmaf_imports(self) -> None:
        """Legacy vmaf imports cover the compatibility package and its shim."""
        for path in (
            "compat/python-vmaf/__init__.py",
            "python/vmaf/__init__.py",
            "compat/python-vmaf/core/__init__.py",
            "compat/python-vmaf/core/result.py",
        ):
            with self.subTest(path=path):
                self.assertEqual(
                    runner.focused_selection(
                        [path], [path], {"test.py": "from vmaf.core.result import Result"}
                    ),
                    (["test.py"], []),
                )

    def test_literal_dynamic_import_selects_unique_hyphenated_stem(self) -> None:
        """Literal dynamic imports cover scripts without a Python identifier stem."""
        path = "scripts/ci/check-codeql-unused-static.py"
        for call in ("importlib.import_module", "import_module", "__import__"):
            for module in ("check-codeql-unused-static", "checks.check-codeql-unused-static"):
                with self.subTest(call=call, module=module):
                    self.assertEqual(
                        runner.focused_selection(
                            [path], [path], {"test.py": f'{call}("{module}")'}
                        ),
                        (["test.py"], []),
                    )

    def test_dynamic_import_nonliteral_argument_does_not_select(self) -> None:
        """Computed module names remain outside literal import discovery."""
        path = "scripts/ci/check-codeql-unused-static.py"
        text = 'name = "check-codeql-" + "unused-static"\nimportlib.import_module(name)'
        self.assertEqual(runner.focused_selection([path], [path], {"test.py": text}), ([], [path]))

    def test_literal_loader_paths_select_hyphenated_script(self) -> None:
        """Loader APIs need no special parsing when their literal paths name the script."""
        path = "scripts/ci/check-codeql-unused-static.py"
        for call in (
            f'importlib.util.spec_from_file_location("checker", ROOT / "{path}")',
            f'SourceFileLoader("checker", "{Path(path).name}")',
            f'runpy.run_path(str(ROOT / "{path}"))',
        ):
            with self.subTest(call=call):
                self.assertEqual(
                    runner.focused_selection([path], [path], {"test.py": call}),
                    (["test.py"], []),
                )

    def test_known_gap_split_basename_is_not_selected(self) -> None:
        """Known gap: separate basename pieces leave no complete path token."""
        path = "scripts/ci/check-codeql-unused-static.py"
        text = 'SCRIPT = ROOT / "scripts" / "ci" / ("check-codeql-" + "unused-static.py")'
        self.assertEqual(runner.focused_selection([path], [path], {"test.py": text}), ([], [path]))

    def test_live_rocm_script_selects_exactly_its_three_tests(self) -> None:
        """Keep the live script-to-test mapping precise across selector changes."""
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
                "scripts/ci/tests/test_fetch_connect_retry.py",
                "scripts/ci/tests/test_install_rocm_from_image.py",
                "scripts/ci/tests/test_required_release_legs.py",
            ],
        )
        self.assertEqual(unnamed, [])

    def test_a_changed_test_selects_itself(self) -> None:
        """Always exercise a changed test even without source references."""
        test = "pkg/tests/test_changed.py"
        self.assertEqual(runner.focused_selection([test], [test], {test: ""}), ([test], []))

    def test_repo_relative_path_selects_with_ambiguous_basename(self) -> None:
        """Preserve full-path coverage when a short name is ambiguous."""
        path = "pkg/meson.build"
        selected, unnamed = runner.focused_selection(
            [path], [path, "other/meson.build"], {"test.py": repr(path)}
        )
        self.assertEqual((selected, unnamed), (["test.py"], []))

    def test_unique_basename_selects(self) -> None:
        """Allow a globally unique short filename to identify its tests."""
        path = ROCM_SCRIPT
        selected, unnamed = runner.focused_selection(
            [path], [path], {"test.py": repr(Path(ROCM_SCRIPT).name)}
        )
        self.assertEqual((selected, unnamed), (["test.py"], []))

    def test_dotted_module_selects(self) -> None:
        """Recognize fully qualified Python references."""
        path = "scripts/ci/check_dco.py"
        selected, unnamed = runner.focused_selection(
            [path], [path], {"test.py": "from scripts.ci.check_dco import main"}
        )
        self.assertEqual((selected, unnamed), (["test.py"], []))

    def test_unique_stem_in_import_selects(self) -> None:
        """Cover short imports while retaining multiline syntax support."""
        path = "scripts/ci/check_dco.py"
        for text in ("import check_dco", "from scripts.ci import (\n    check_dco,\n)"):
            with self.subTest(text=text):
                self.assertEqual(
                    runner.focused_selection([path], [path], {"test.py": text}),
                    (["test.py"], []),
                )

    def test_nonunique_basename_selects_nothing(self) -> None:
        """Prevent generic filenames from claiming unrelated tests."""
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
        """Reject suffixes and longer filenames that merely contain a path."""
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
        """Support repository paths joined to a root or used in prose."""
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
        """Recognize a unique basename in a relative path."""
        path = ROCM_SCRIPT
        text = "HERE / '../" + ROCM_NAME + "'"
        self.assertEqual(
            runner.focused_selection([path], [path], {"test.py": text}), (["test.py"], [])
        )

    def test_dotted_module_attribute_selects(self) -> None:
        """Retain coverage through references to module attributes."""
        path = "scripts/ci/check_dco.py"
        text = "mock.patch('scripts.ci.check_dco.run')"
        self.assertEqual(
            runner.focused_selection([path], [path], {"test.py": text}), (["test.py"], [])
        )

    def test_stem_in_prose_or_alias_does_not_select(self) -> None:
        """Require import identity rather than incidental short-name text."""
        path = "scripts/ci/check_dco.py"
        for text in ("# check_dco", "import other as check_dco", "# invalid Python\n!check_dco"):
            with self.subTest(text=text):
                self.assertEqual(
                    runner.focused_selection([path], [path], {"test.py": text}), ([], [path])
                )

    def test_nonunique_stem_in_import_does_not_select(self) -> None:
        """Refuse short imports shared by multiple source modules."""
        path = "pkg/common.py"
        self.assertEqual(
            runner.focused_selection(
                [path], [path, "other/common.py"], {"test.py": "from pkg import common"}
            ),
            ([], [path]),
        )

    def test_longer_module_does_not_select(self) -> None:
        """Keep module token boundaries precise."""
        path = "pkg/mod.py"
        self.assertEqual(
            runner.focused_selection([path], [path], {"test.py": "# pkg.mod_extra"}),
            ([], [path]),
        )


class PushHeadTests(unittest.TestCase):
    def test_equal_outgoing_ref_passes_for_both_framework_variables(self) -> None:
        """Accept each supported framework ref without leaking Git overrides."""
        for variable in ("PRE_COMMIT_TO_REF", "PRE_COMMIT_LOCAL_BRANCH"):
            with (
                self.subTest(variable=variable),
                patch.object(
                    os,
                    "environ",
                    {variable: "pushed", "GIT_DIR": "/unrelated", "PATH": os.environ["PATH"]},
                ),
                patch.object(push_head, "run_command") as command,
            ):
                command.return_value.returncode = 0
                command.return_value.stdout = "same\n"
                push_head.outgoing_head("same", ROOT)
                self.assertEqual(
                    command.call_args.kwargs["env"],
                    {variable: "pushed", "PATH": os.environ["PATH"]},
                )
                self.assertEqual(command.call_args.args[0][-1], "pushed^{commit}")

    def test_differing_ref_raises(self) -> None:
        """Refuse validation of a branch other than the one being pushed."""
        with (
            patch.object(
                os, "environ", {"PRE_COMMIT_TO_REF": "pushed", "PATH": os.environ["PATH"]}
            ),
            patch.object(push_head, "run_command") as command,
        ):
            command.return_value.returncode = 0
            command.return_value.stdout = "other\n"
            with self.assertRaisesRegex(RuntimeError, "outgoing ref differs from HEAD"):
                push_head.outgoing_head("head", ROOT)

    def test_unset_outgoing_ref_passes_without_git(self) -> None:
        """Allow direct diagnostic runs without push framework metadata."""
        with (
            patch.object(os, "environ", {}),
            patch.object(push_head, "run_command") as command,
        ):
            push_head.outgoing_head("head", ROOT)
            command.assert_not_called()

    def test_missing_outgoing_ref_fails_closed(self) -> None:
        """Refuse unresolved outgoing refs before test execution."""
        with (
            patch.object(
                os, "environ", {"PRE_COMMIT_TO_REF": "missing", "PATH": os.environ["PATH"]}
            ),
            patch.object(push_head, "run_command") as command,
        ):
            command.return_value.returncode = 128
            command.return_value.stderr = "missing"
            with self.assertRaisesRegex(RuntimeError, "cannot resolve outgoing ref"):
                push_head.outgoing_head("head", ROOT)


class VenvKeyTests(unittest.TestCase):
    def setUp(self) -> None:
        """Build a temporary root holding a suite manifest for venv key cases."""
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
        """A skip naming a declared suite input is refused only when the suite declares it."""
        skips = [("t::a", "vmaf binary not found")]
        self.assertEqual(runner.skip_violations(self.suite(None), skips), [])
        self.assertEqual(len(runner.skip_violations(self.suite("vmaf binary"), skips)), 1)

    def test_an_unrelated_skip_is_not_a_violation(self) -> None:
        """A skip unrelated to packages or declared inputs is no violation."""
        self.assertEqual(runner.skip_violations(self.suite(None), [("t::a", "needs a GPU")]), [])


@unittest.skipIf(GIT is None, "git is required to build the fixture repositories")
class RunTests(unittest.TestCase):
    def setUp(self) -> None:
        """Isolate subprocess integration fixtures from the caller checkout."""
        self._tmp = tempfile.TemporaryDirectory(prefix="affected-run-")
        self.addCleanup(self._tmp.cleanup)
        self.repo = Path(self._tmp.name)
        self.env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
        self.git("init", "-q")
        self.parent: str | None = None

    def git(self, *args: str, input_data: str | None = None) -> str:
        """Bound fixture Git operations and propagate their failures."""
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
        """Provide committed fixture inputs for checkout identity checks."""
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
        self.commit()

    def run_script(self, *args: str) -> TextCommandResult:
        """Exercise the CLI with a local interpreter and no installer."""
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
        """Expose unavailable local coverage and enforce strict skip policy."""
        self.build({"pkg/tests/test_ok.py": "def test_ok():\n    assert True\n"})
        lenient = self.run_script("--files", "cmd/x.go")
        self.assertEqual(lenient.returncode, 0, lenient.stderr)
        self.assertIn("go: NOT RUN", lenient.stdout)
        self.assertIn("needs the Go build", lenient.stdout)
        self.assertIn("alternate coverage: pull request CI jobs: Go", lenient.stdout)
        strict = self.run_script("--strict", "--files", "cmd/x.go")
        self.assertEqual(strict.returncode, 1)

    def test_base_and_head_select_the_files_that_differ(self) -> None:
        """Derive affected suites from a real committed change range."""
        self.build({"pkg/tests/test_ok.py": "def test_ok():\n    assert True\n"})
        self.commit()
        (self.repo / "pkg" / "tests" / "test_ok.py").write_text("def test_ok():\n    assert 1\n")
        self.git("add", "-A")
        self.commit()
        result = self.run_script("--list", "--base", "HEAD~1", "--head", "HEAD")
        self.assertIn("pkg: 1 changed file(s), e.g. pkg/tests/test_ok.py", result.stdout)

    def test_focused_unnamed_file_is_not_run_or_counted_passed(self) -> None:
        """Keep uncovered changes visible with their alternate CI coverage."""
        self.build({"pkg/tests/test_bad.py": "def test_bad():\n    assert False\n"})
        result = self.run_script("--focused", "--files", LOCK)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f"NOT RUN {LOCK}: no test names this file", result.stdout)
        self.assertIn("pkg: NOT RUN (no test names a changed file)", result.stdout)
        self.assertIn(
            f"NOT RUN {LOCK}: no test names this file; alternate coverage: pull request CI jobs: Pkg",
            result.stdout,
        )
        self.assertIn("    alternate coverage: pull request CI jobs: Pkg", result.stdout)
        self.assertNotIn("passed", result.stdout)
        strict = self.run_script("--strict", "--focused", "--files", LOCK)
        self.assertEqual(strict.returncode, 1, strict.stdout + strict.stderr)

    def test_focused_changed_test_runs_only_itself(self) -> None:
        """Avoid executing unrelated tests in focused mode."""
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
        """Preserve uncovered-path reporting alongside successful test execution."""
        self.build({"pkg/tests/test_ok.py": "def test_ok():\n    assert True\n"})
        result = self.run_script("--focused", "--files", f"{LOCK},pkg/tests/test_ok.py")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f"NOT RUN {LOCK}: no test names this file", result.stdout)
        self.assertIn("pkg: passed: 1 passed", result.stdout)

    def test_dirty_selected_test_fails_closed(self) -> None:
        """Unstaged and staged test edits cannot validate a different pushed test."""
        test = "pkg/tests/test_ok.py"
        body = f"# {LOCK}\ndef test_ok():\n    assert True\n"
        self.build({test: body})
        (self.repo / test).write_text(body.replace("True", "False"), encoding="utf-8")
        for staged in (False, True):
            with self.subTest(staged=staged):
                if staged:
                    self.git("add", test)
                result = self.run_script("--focused", "--files", LOCK)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertIn(test, result.stderr)
                self.assertIn("differs from HEAD", result.stderr)
                self.assertNotIn("pkg: FAILED", result.stdout)

    def test_dirty_registry_fails_closed(self) -> None:
        """Registry edits cannot hide suites in focused or full-suite execution."""
        test = "pkg/tests/test_ok.py"
        self.build({test: "def test_ok():\n    assert True\n"})
        registry = registry_lib.MANIFEST.as_posix()
        data = manifest()
        data["suites"] = [
            {
                "name": "other",
                "paths": ["other/"],
                "checks": ["Other"],
                "not_local": "needs another build",
            }
        ]
        (self.repo / registry).write_text(json.dumps(data), encoding="utf-8")
        for staged in (False, True):
            if staged:
                self.git("add", registry)
            for mode in ((), ("--focused",)):
                with self.subTest(staged=staged, mode=mode):
                    result = self.run_script(*mode, "--files", test)
                    self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                    self.assertIn(registry, result.stderr)
                    self.assertIn("differs from HEAD", result.stderr)
                    self.assertNotIn("passed:", result.stdout)

    def test_dirty_registry_list_reads_worktree(self) -> None:
        """Listing may inspect local registry changes without executing tests."""
        test = "pkg/tests/test_ok.py"
        self.build({test: "def test_ok():\n    assert True\n"})
        data = manifest()
        data["suites"] = [
            {
                "name": "other",
                "paths": ["other/"],
                "checks": ["Other"],
                "not_local": "needs another build",
            }
        ]
        (self.repo / registry_lib.MANIFEST).write_text(json.dumps(data), encoding="utf-8")
        for mode in ((), ("--focused",)):
            with self.subTest(mode=mode):
                result = self.run_script(*mode, "--list", "--files", test)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(result.stderr, "")
                self.assertNotIn("pkg:", result.stdout)
                self.assertNotIn("passed:", result.stdout)

    def test_dirty_candidate_cannot_hide_head_reference(self) -> None:
        """Removing a HEAD reference still selects and refuses the dirty candidate."""
        test = "pkg/tests/test_ok.py"
        body = "def test_ok():\n    assert True\n"
        self.build({test: f"# {LOCK}\n{body}"})
        (self.repo / test).write_text(body, encoding="utf-8")
        for staged in (False, True):
            with self.subTest(staged=staged):
                if staged:
                    self.git("add", test)
                result = self.run_script("--focused", "--files", LOCK)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertIn("pkg: selected 1 test file(s)", result.stdout)
                self.assertIn(test, result.stderr)
                self.assertIn("differs from HEAD", result.stderr)
                self.assertNotIn("passed:", result.stdout)

    def test_dirty_unrelated_candidate_does_not_block(self) -> None:
        """Unselected candidates may differ from HEAD without invalidating coverage."""
        test = "pkg/tests/test_ok.py"
        unrelated = "pkg/tests/test_other.py"
        self.build(
            {
                test: f"# {LOCK}\ndef test_ok():\n    assert True\n",
                unrelated: "def test_other():\n    assert True\n",
            }
        )
        (self.repo / unrelated).write_text(
            "def test_other():\n    assert False\n", encoding="utf-8"
        )
        for staged in (False, True):
            with self.subTest(staged=staged):
                if staged:
                    self.git("add", unrelated)
                result = self.run_script("--focused", "--files", LOCK)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("pkg: selected 1 test file(s)", result.stdout)
                self.assertIn("pkg: passed: 1 passed", result.stdout)

    def test_index_only_candidate_is_not_selected(self) -> None:
        """A staged new candidate cannot supply references absent from HEAD."""
        self.build({"pkg/tests/test_ok.py": "def test_ok():\n    assert True\n"})
        test = "pkg/tests/test_new.py"
        (self.repo / test).write_text(f"# {LOCK}\ndef test_new():\n    assert False\n")
        self.git("add", test)
        result = self.run_script("--focused", "--files", LOCK)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("pkg: selected 0 test file(s)", result.stdout)
        self.assertNotIn("passed:", result.stdout)

    def test_head_candidates_use_one_binary_batch(self) -> None:
        """Byte lengths preserve Unicode, invalid UTF-8 and delimiters across candidate blobs."""
        first = "pkg/tests/test_first.py"
        second = "pkg/tests/test_second.py"
        absent = "pkg/tests/test_absent.py"
        body = "# snowman: \u2603\r\n# embedded: \0\n"
        self.build({first: body, second: "# initial\n"})
        (self.repo / second).write_bytes(b"# invalid: \xff\n")
        self.git("add", second)
        self.commit()
        with patch.object(runner, "_git", wraps=runner._git) as command:
            texts = runner._head_test_texts(self.repo, [first, absent, second])
        self.assertEqual(texts, {first: body, second: "# invalid: \ufffd\n"})
        command.assert_called_once()
        self.assertNotIn("-Z", command.call_args.args)

    def test_head_candidate_with_newline_is_refused_before_git(self) -> None:
        """A newline cannot be framed in the batch, so the path fails closed without Git."""
        with patch.object(runner, "_git") as command:
            with self.assertRaisesRegex(runner.RunnerError, "contains a newline"):
                runner._head_test_texts(self.repo, ["pkg/tests/test_a\nb.py"])
        command.assert_not_called()

    def test_failed_binary_git_request_reports_decoded_stderr(self) -> None:
        """Binary and text Git failures both name the command with readable stderr."""
        for input_data in (b"HEAD:missing\n", None):
            with self.subTest(binary=input_data is not None):
                with self.assertRaises(runner.RunnerError) as caught:
                    runner._git(self.repo, "cat-file", "--no-such-option", input_data=input_data)
                message = str(caught.exception)
                self.assertIn("git cat-file --no-such-option failed: ", message)
                detail = message.split("failed: ", 1)[1]
                self.assertFalse(detail.startswith(("b'", 'b"')), detail[:40])

    def test_dirty_changed_path_fails_closed_even_without_selected_tests(self) -> None:
        """Changed source paths must match HEAD even when no test names them."""
        self.build({"pkg/tests/test_ok.py": "def test_ok():\n    assert True\n"})
        (self.repo / LOCK).write_text("# dirty lock\n", encoding="utf-8")
        result = self.run_script("--focused", "--files", LOCK)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn(LOCK, result.stderr)

    def test_dirty_index_fails_when_worktree_matches_head(self) -> None:
        """A restored worktree cannot conceal staged edits to the selected test."""
        test = "pkg/tests/test_ok.py"
        body = "def test_ok():\n    assert True\n"
        self.build({test: body})
        (self.repo / test).write_text(body.replace("True", "False"), encoding="utf-8")
        self.git("add", test)
        (self.repo / test).write_text(body, encoding="utf-8")
        result = self.run_script("--focused", "--files", test)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn(test, result.stderr)

    def test_dirty_unrelated_tracked_and_untracked_files_do_not_block(self) -> None:
        """Unrelated local edits do not invalidate a clean selected test."""
        test = "pkg/tests/test_ok.py"
        self.build({test: "def test_ok():\n    assert True\n", "notes.txt": "clean\n"})
        (self.repo / "notes.txt").write_text("dirty\n", encoding="utf-8")
        (self.repo / "untracked.txt").write_text("untracked\n", encoding="utf-8")
        result = self.run_script("--focused", "--files", test)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("pkg: passed: 1 passed", result.stdout)

    def test_untracked_changed_path_fails_closed(self) -> None:
        """An explicitly selected untracked source cannot stand in for absent HEAD content."""
        self.build({"pkg/tests/test_ok.py": "# new.cfg\ndef test_ok():\n    assert True\n"})
        (self.repo / "new.cfg").write_text("local only\n", encoding="utf-8")
        result = self.run_script("--focused", "--files", "new.cfg")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("new.cfg", result.stderr)

    def test_untriggered_root_config_names_a_test_in_local_suite(self) -> None:
        """Unmapped root files can discover their tests without changing CI triggers."""
        self.build(
            {"pkg/tests/test_ok.py": "# .pre-commit-config.yaml\ndef test_ok():\n    assert True\n"}
        )
        result = self.run_script("--focused", "--files", ".pre-commit-config.yaml")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("pkg: passed: 1 passed", result.stdout)
        self.assertNotIn("NOT RUN .pre-commit-config.yaml", result.stdout)

    def test_unnamed_untriggered_path_reports_reason_and_coverage(self) -> None:
        """Unmapped and unnamed files remain visible with discovered CI coverage."""
        self.build({"pkg/tests/test_ok.py": "def test_ok():\n    assert True\n"})
        result = self.run_script("--focused", "--files", "unnamed.cfg")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("NOT RUN unnamed.cfg: no test names this file", result.stdout)
        self.assertIn("alternate coverage: pull request CI jobs:", result.stdout)
        self.assertIn("Pkg", result.stdout)
        strict = self.run_script("--strict", "--focused", "--files", "unnamed.cfg")
        self.assertEqual(strict.returncode, 1, strict.stdout + strict.stderr)

    def test_untriggered_path_cannot_discover_tests_in_nonlocal_suite(self) -> None:
        """Fallback discovery only searches locally runnable suites, even alongside triggers."""
        self.build({"cmd/test_config.py": "# unnamed.cfg\n"})
        result = self.run_script("--list", "--focused", "--files", "cmd/new.go,unnamed.cfg")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("NOT RUN unnamed.cfg: no test names this file", result.stdout)
        self.assertNotIn("go: cmd/test_config.py", result.stdout)

    def test_deleted_unique_stem_import_uses_base_tree(self) -> None:
        """A deleted module retains its base-tree import identity."""
        path = "ai/src/vanished.py"
        test = "ai/tests/test_removed.py"
        self.build({path: "VALUE = 1\n", test: "from vanished import VALUE\n"})
        self.git("rm", path)
        self.commit()
        result = self.run_script("--list", "--focused", "--base", "HEAD~1", "--head", "HEAD")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f"ai: {test}", result.stdout)
        self.assertNotIn(f"NOT RUN {path}", result.stdout)

    def test_deleted_ambiguous_stem_does_not_become_unique(self) -> None:
        """Base-tree collisions survive deletion and still require a full path."""
        path = "ai/src/common.py"
        test = "ai/tests/test_removed.py"
        self.build({path: "", "other/common.py": "", test: "import common\n"})
        self.git("rm", path)
        self.commit()
        result = self.run_script("--list", "--focused", "--base", "HEAD~1", "--head", "HEAD")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f"NOT RUN {path}: no test names this file", result.stdout)
        self.assertNotIn(f"ai: {test}", result.stdout)

    def test_merge_base_conflicting_modes_are_usage_errors(self) -> None:
        """Reject competing sources of changed-file scope."""
        for args in (("--files", "x"), ("--base", "x", "--head", "y"), ("--head", "x")):
            with self.subTest(args=args):
                self.assertEqual(self.run_script("--merge-base", "HEAD", *args).returncode, 2)

    def test_merge_base_uses_head_and_checks_outgoing_ref(self) -> None:
        """Validate merge-base changes only for the outgoing checkout."""
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

    def test_focused_head_must_be_the_checkout(self) -> None:
        """Focused selection reads tests from HEAD, so another --head is refused."""
        self.build({"pkg/tests/test_ok.py": "def test_ok():\n    assert True\n"})
        self.commit()
        (self.repo / "pkg/widget.py").write_text("VALUE = 1\n", encoding="utf-8")
        self.git("add", "-A")
        self.commit()
        head = self.git("rev-parse", "HEAD").strip()
        for args in (("HEAD~1", "HEAD"), ("HEAD~1", head)):
            with self.subTest(head=args[1]):
                good = self.run_script("--list", "--focused", "--base", args[0], "--head", args[1])
                self.assertEqual(good.returncode, 0, good.stdout + good.stderr)
        bad = self.run_script("--list", "--focused", "--base", "HEAD~2", "--head", "HEAD~1")
        self.assertEqual(bad.returncode, 2, bad.stdout + bad.stderr)
        self.assertIn("--focused reads tests from HEAD", bad.stderr)
        ranged = self.run_script("--list", "--base", "HEAD~2", "--head", "HEAD~1")
        self.assertEqual(ranged.returncode, 0, ranged.stdout + ranged.stderr)

    def test_base_without_head_is_a_usage_error(self) -> None:
        self.build({})
        self.assertEqual(self.run_script("--base", "HEAD").returncode, 2)


if __name__ == "__main__":
    unittest.main()
