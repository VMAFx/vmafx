# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Migration fixtures never touch the real train or execute GitHub requests."""

from __future__ import annotations

import importlib.util
import json
import os
import re
import shutil
import stat
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import install_merge_train_guard as installer
import merge_train_guard as guard

from scripts.lib.safe_subprocess import CommandFailed


def library_digests(value: dict[str, object]) -> dict[str, str]:
    """Return the preview's library digest map; a non-mapping fails the test."""
    digests = value["library_sha256"]
    if not isinstance(digests, dict):
        raise AssertionError(f"library_sha256 is {type(digests).__name__}, not a mapping")
    return digests


class MigrationContract(unittest.TestCase):
    def isolate_git_config(self) -> None:
        """Both Git paths run without the caller's global and system config."""
        base = guard.environment
        isolated = patch.object(
            guard,
            "environment",
            side_effect=lambda: dict(base(), GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1"),
        )
        isolated.start()
        self.addCleanup(isolated.stop)
        original_run_command = installer.run_command

        def isolated_run_command(*args: Any, **kwargs: Any) -> Any:
            passed_env = kwargs.get("env")
            env = dict(
                (
                    passed_env
                    if passed_env is not None
                    else {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
                ),
                GIT_CONFIG_GLOBAL=os.devnull,
                GIT_CONFIG_NOSYSTEM="1",
            )
            kwargs["env"] = env
            return original_run_command(*args, **kwargs)

        isolated_run = patch.object(installer, "run_command", side_effect=isolated_run_command)
        isolated_run.start()
        self.addCleanup(isolated_run.stop)

    def setUp(self) -> None:
        """Build temporary Git repository with committed scripts and state directory fixtures."""
        self.isolate_git_config()
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.source = self.directory / "source"
        self.source.mkdir()
        installer.git(self.source, "init", "--initial-branch=master")
        installer.git(self.source, "config", "user.name", "Migration fixture")
        installer.git(self.source, "config", "user.email", "fixture@example.invalid")
        installer.git(self.source, "config", "core.hooksPath", "/dev/null")
        installer.git(self.source, "config", "commit.gpgsign", "false")
        gateway = self.source / installer.SOURCE_PATH
        gateway.parent.mkdir(parents=True)
        gateway.write_text(
            "import json, sys\n"
            "from pathlib import Path\n"
            "def load_squash_composer():\n"
            "    import importlib.util\n"
            "    p = Path(__file__).resolve().parent / 'squash_message.py'\n"
            "    spec = importlib.util.spec_from_file_location('squash_message', str(p))\n"
            "    m = importlib.util.module_from_spec(spec)\n"
            "    spec.loader.exec_module(m)\n"
            "    return m\n"
            "if __name__ == '__main__':\n"
            "    print(json.dumps(sys.argv[1:]))\n"
        )
        composer = self.source / installer.COMPOSER_PATH
        composer.parent.mkdir(parents=True, exist_ok=True)
        composer.write_text("TEST_COMPOSER = True\n")
        library = self.source / "scripts/lib"
        library.mkdir()
        (library / "__init__.py").write_text("# Fixture library package.\n")
        installer.git(self.source, "add", ".")
        installer.git(self.source, "commit", "-m", "fixture gateway and composer")
        self.state = self.directory / "state with spaces"
        self.state.mkdir()
        for name in installer.RUNTIME_FILES:
            (self.state / name).write_text(f"original {name}\n")
        (self.state / "hold.txt").write_text("1396 human hold\n1416 owner\n1421 stack\n")
        (self.state / "policy.json").write_text(
            json.dumps({"schema": 1, "protected_branches": ["existing"], "protected_worktrees": []})
        )
        self.before = {
            name: (self.state / name).read_bytes()
            for name in (*installer.RUNTIME_FILES, "hold.txt", "policy.json")
        }

    def plan(self) -> tuple[dict[str, object], bytes]:
        return installer.plan(
            self.source,
            self.source,
            self.state,
            ["active-root"],
            [str(self.directory / "agent-active")],
        )

    def real_gateway(self) -> None:
        """Commit real runtime sources; adapters still target only the disposable repository."""
        checkout = Path(__file__).resolve().parents[3]
        for relative in (
            installer.SOURCE_PATH,
            installer.COMPOSER_PATH,
            "scripts/lib/__init__.py",
            "scripts/lib/safe_subprocess.py",
        ):
            shutil.copyfile(checkout / relative, self.source / relative)
        installer.git(self.source, "add", ".")
        installer.git(self.source, "commit", "-m", "real gateway and runtime dependencies")

    def assert_adapter_status(self, name: str, result: Any) -> None:
        """Without a PR number rebase-clean.sh refuses with 1; the other adapters exit 0."""
        self.assertEqual(result.returncode, 1 if name == "rebase-clean.sh" else 0, result.stderr)

    def run_adapter(self, name: str, cwd: Path, *, direct: bool = False) -> Any:
        """Run installed entry points with offline gh and isolated fixture Git configuration.

        ``direct`` executes the Python operator through its own shebang, as the
        shell adapters always are, instead of through an explicit interpreter.
        """
        binaries = self.directory / "bin"
        binaries.mkdir(exist_ok=True)
        gh = binaries / "gh"
        gh.write_text("#!/bin/sh\nprintf '[]\\n'\n")
        gh.chmod(0o755)
        env = dict(
            guard.environment(),
            PATH=str(binaries) + os.pathsep + os.environ.get("PATH", ""),
            GIT_CONFIG_GLOBAL=os.devnull,
            GIT_CONFIG_NOSYSTEM="1",
            CUDA_VISIBLE_DEVICES="",
        )
        adapter = self.state / name
        explicit = name.endswith(".py") and not direct
        argv = [sys.executable, str(adapter)] if explicit else [str(adapter)]
        return installer.run_command(
            argv,
            allowed_executables=(argv[0],),
            cwd=cwd,
            env=env,
            capture_output=True,
            text=True,
            timeout_seconds=30,
        )

    def test_real_gateway_adapters_start_outside_checkout(self) -> None:
        self.real_gateway()
        value, committed = self.plan()
        installer.install(value, committed, installer.plan_hash(value))
        outside = self.directory / "empty caller"
        outside.mkdir()
        for name in ("rebase-clean.sh", "train.sh", "merge_train_operator.py"):
            with self.subTest(adapter=name):
                result = self.run_adapter(name, outside)
                self.assertNotIn("ModuleNotFoundError", result.stderr)
                self.assertNotIn("ImportError", result.stderr)
                if name == "rebase-clean.sh":
                    self.assertEqual(result.returncode, 1, result.stderr)
                    self.assertIn("an explicit positive PR number is required", result.stderr)
                else:
                    self.assertEqual(result.returncode, 0, result.stderr)

    def test_real_gateway_ignores_planted_caller_and_adapter_libraries(self) -> None:
        self.real_gateway()
        value, committed = self.plan()
        installer.install(value, committed, installer.plan_hash(value))
        caller = self.directory / "planted caller"
        caller.mkdir()
        ambient = self.directory / "planted import path"
        for root in (caller, self.state, ambient):
            library = root / "scripts/lib"
            library.mkdir(parents=True)
            for relative in (
                "scripts/__init__.py",
                "scripts/lib/__init__.py",
                "scripts/lib/safe_subprocess.py",
            ):
                (root / relative).write_text("raise RuntimeError('PLANTED_LIBRARY_LOADED')\n")
        for name in ("rebase-clean.sh", "train.sh", "merge_train_operator.py"):
            with self.subTest(adapter=name), patch.dict(os.environ, {"PYTHONPATH": str(ambient)}):
                result = self.run_adapter(name, caller)
                self.assertNotIn("PLANTED_LIBRARY_LOADED", result.stderr)
                self.assertNotIn("Traceback", result.stderr)
                self.assert_adapter_status(name, result)

    def test_adapters_ignore_planted_standard_library_modules(self) -> None:
        self.real_gateway()
        value, committed = self.plan()
        installer.install(value, committed, installer.plan_hash(value))
        caller = self.directory / "planted caller"
        ambient = self.directory / "planted import path"
        caller.mkdir()
        ambient.mkdir()
        for root in (caller, self.state, ambient):
            (root / "hashlib.py").write_text("raise RuntimeError('PLANTED_STDLIB_LOADED')\n")
        for name in ("rebase-clean.sh", "train.sh", "merge_train_operator.py"):
            with self.subTest(adapter=name), patch.dict(os.environ, {"PYTHONPATH": str(ambient)}):
                result = self.run_adapter(name, caller)
                self.assertNotIn("PLANTED_STDLIB_LOADED", result.stderr)
                self.assertNotIn("Traceback", result.stderr)
                self.assert_adapter_status(name, result)

    def test_executed_adapters_start_isolated_before_site_hooks(self) -> None:
        """A planted sitecustomize.py runs at interpreter start, before any re-execution."""
        self.real_gateway()
        value, committed = self.plan()
        installer.install(value, committed, installer.plan_hash(value))
        caller = self.directory / "planted caller"
        ambient = self.directory / "planted import path"
        marker = self.directory / "SITE_HOOK_RAN"
        caller.mkdir()
        ambient.mkdir()
        hook = f"open({str(marker)!r}, 'a').write('ran\\n')\n"
        for root in (caller, self.state, ambient):
            (root / "sitecustomize.py").write_text(hook)
        operator = (self.state / "merge_train_operator.py").read_text().splitlines()[0]
        self.assertEqual(operator, "#!/usr/bin/env -S python3 -I")
        for name in ("rebase-clean.sh", "train.sh", "merge_train_operator.py"):
            with self.subTest(adapter=name), patch.dict(os.environ, {"PYTHONPATH": str(ambient)}):
                result = self.run_adapter(name, caller, direct=True)
                self.assertFalse(marker.exists(), f"{name} ran a planted sitecustomize.py")
                self.assertNotIn("Traceback", result.stderr)
                self.assert_adapter_status(name, result)

    def test_library_manifest_includes_transitive_imports_and_package_initializers(self) -> None:
        sources = {
            installer.SOURCE_PATH: (
                "import scripts.lib.first as first\nfrom scripts.lib.second import value\n"
                "from scripts.lib import third\nfrom scripts.lib.pkg import leaf\n"
            ),
            "scripts/lib/__init__.py": "import scripts.lib.bootstrap\n",
            "scripts/lib/bootstrap.py": "# Package initialization dependency.\n",
            "scripts/lib/first.py": "from scripts.lib.second import value\n",
            "scripts/lib/second.py": "import scripts.lib.first\nvalue = 1\n",
            "scripts/lib/third.py": "value = 3\n",
            "scripts/lib/pkg.py": "raise RuntimeError('package takes precedence')\n",
            "scripts/lib/pkg/__init__.py": "import scripts.lib.third\n",
            "scripts/lib/pkg/leaf.py": "value = 4\n",
        }
        for relative, content in sources.items():
            target = self.source / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content)
        installer.git(self.source, "add", ".")
        installer.git(self.source, "commit", "-m", "transitive library fixture")
        value, committed = self.plan()
        expected = {
            relative: installer.sha256(content.encode())
            for relative, content in sorted(sources.items())
            if relative.startswith("scripts/lib/") and relative != "scripts/lib/pkg.py"
        }
        self.assertEqual(value["library_sha256"], expected)
        self.assertEqual(list(library_digests(value)), sorted(expected))
        installer.install(value, committed, installer.plan_hash(value))
        release = self.state / "releases" / installer.release_name(value)
        for relative, digest in expected.items():
            target = release / relative
            self.assertEqual(installer.sha256(target.read_bytes()), digest)
            self.assertEqual(stat.S_IMODE(target.stat().st_mode), 0o444)

    def test_no_library_imports_ships_only_package_initializer(self) -> None:
        (self.source / "scripts/lib/unused.py").write_text("raise RuntimeError('unused')\n")
        installer.git(self.source, "add", ".")
        installer.git(self.source, "commit", "-m", "unreferenced library module")
        value, committed = self.plan()
        relative = "scripts/lib/__init__.py"
        self.assertEqual(
            value["library_sha256"],
            {relative: installer.sha256((self.source / relative).read_bytes())},
        )
        installer.install(value, committed, installer.plan_hash(value))
        release = self.state / "releases" / installer.release_name(value)
        self.assertTrue((release / relative).is_file())
        self.assertFalse((release / "scripts/lib/unused.py").exists())

    def commit_sources(self, sources: dict[str, str], message: str) -> None:
        for relative, content in sources.items():
            target = self.source / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content)
        installer.git(self.source, "add", ".")
        installer.git(self.source, "commit", "-m", message)

    def test_relative_library_import_refuses_preview(self) -> None:
        self.commit_sources(
            {
                "scripts/lib/helper.py": "value = 1\n",
                "scripts/lib/pkg/__init__.py": "",
                "scripts/lib/pkg/sibling.py": "value = 2\n",
            },
            "relative import targets",
        )
        cases = (
            ("scripts/lib/user.py", "from . import helper\n"),
            ("scripts/lib/user.py", "from .helper import value\n"),
            ("scripts/lib/pkg/user.py", "from ..helper import value\n"),
            ("scripts/lib/pkg/user.py", "from .sibling import value\n"),
        )
        for relative, content in cases:
            with self.subTest(module=relative, source=content):
                module = relative.removesuffix(".py").replace("/", ".")
                self.commit_sources(
                    {installer.SOURCE_PATH: f"import {module}\n", relative: content},
                    "relative import fixture",
                )
                with self.assertRaisesRegex(
                    ValueError, re.escape(f"relative import in {relative}:1")
                ):
                    self.plan()

    def test_scripts_import_outside_library_refuses_preview(self) -> None:
        self.commit_sources(
            {"scripts/ci/helper.py": "value = 1\n", "scripts/dev/tool.py": "value = 2\n"},
            "checkout-only modules",
        )
        cases = (
            ("scripts/lib/user.py", "import scripts.dev.tool\n", "scripts.dev.tool"),
            ("scripts/lib/user.py", "from scripts.ci import helper\n", "scripts.ci"),
            ("scripts/lib/user.py", "from scripts.ci.helper import value\n", "scripts.ci.helper"),
            (installer.SOURCE_PATH, "from scripts.dev import tool\n", "scripts.dev"),
        )
        for relative, content, name in cases:
            with self.subTest(module=relative, source=content):
                sources = {relative: content}
                if relative != installer.SOURCE_PATH:
                    sources[installer.SOURCE_PATH] = "import scripts.lib.user\n"
                self.commit_sources(sources, "outside-library import fixture")
                with self.assertRaisesRegex(
                    ValueError, re.escape(f"{relative} imports {name} outside scripts.lib")
                ):
                    self.plan()

    def test_dynamic_import_refuses_preview(self) -> None:
        cases = (
            ('__import__("scripts.ci.helper")\n', 1),
            ('import importlib\nimportlib.import_module("scripts.lib.extra")\n', 2),
            (
                'from importlib import import_module\nname = "scripts.lib.extra"\n'
                "import_module(name)\n",
                3,
            ),
            ('import builtins\nbuiltins.__import__("x")\n', 2),
        )
        for relative in (installer.SOURCE_PATH, "scripts/lib/user.py"):
            for content, line in cases:
                with self.subTest(module=relative, source=content):
                    sources = {relative: content}
                    if relative != installer.SOURCE_PATH:
                        sources[installer.SOURCE_PATH] = "import scripts.lib.user\n"
                    self.commit_sources(sources, "dynamic import fixture")
                    message = (
                        f"dynamic import in {relative}:{line}; "
                        "the release ships static scripts.lib imports only"
                    )
                    with self.assertRaisesRegex(ValueError, f"^{re.escape(message)}$"):
                        self.plan()

    def test_path_loader_and_similarly_named_calls_allow_preview(self) -> None:
        cases = (
            "import importlib.util\n"
            "spec = importlib.util.spec_from_file_location('extra', 'extra.py')\n"
            "module = importlib.util.module_from_spec(spec)\n"
            "spec.loader.exec_module(module)\n",
            "obj.import_modules()\n",
        )
        for relative in (installer.SOURCE_PATH, "scripts/lib/user.py"):
            for content in cases:
                with self.subTest(module=relative, source=content):
                    sources = {relative: content}
                    expected = ["scripts/lib/__init__.py"]
                    if relative != installer.SOURCE_PATH:
                        sources[installer.SOURCE_PATH] = "import scripts.lib.user\n"
                        expected.append(relative)
                    self.commit_sources(sources, "allowed call fixture")
                    value, _ = self.plan()
                    self.assertEqual(sorted(library_digests(value)), expected)

    def test_scripts_package_and_library_names_still_resolve(self) -> None:
        self.commit_sources(
            {
                installer.SOURCE_PATH: (
                    "import scripts\nfrom scripts import lib\nfrom scripts.lib.user import value\n"
                ),
                "scripts/lib/user.py": "value = 1\n",
            },
            "boundary library imports",
        )
        value, _ = self.plan()
        self.assertEqual(
            sorted(library_digests(value)), ["scripts/lib/__init__.py", "scripts/lib/user.py"]
        )

    def test_library_source_must_match_committed_blob(self) -> None:
        self.real_gateway()
        for relative in ("scripts/lib/__init__.py", "scripts/lib/safe_subprocess.py"):
            with self.subTest(module=relative):
                original = (self.source / relative).read_bytes()
                (self.source / relative).write_bytes(original + b"# Uncommitted.\n")
                with self.assertRaisesRegex(ValueError, "library.*committed Git blob"):
                    self.plan()
                (self.source / relative).write_bytes(original)

    def test_missing_committed_import_and_invalid_syntax_refuse_preview(self) -> None:
        for content, message in (
            ("import scripts.lib.missing\n", "missing committed library module"),
            ("from scripts.lib import (\n", "cannot parse imports"),
        ):
            with self.subTest(source=content):
                (self.source / installer.SOURCE_PATH).write_text(content)
                installer.git(self.source, "commit", "-am", "invalid import fixture")
                with self.assertRaisesRegex(ValueError, message):
                    self.plan()

    def test_library_bytes_rechecked_against_preview_before_install(self) -> None:
        self.real_gateway()
        value, committed = self.plan()
        original = installer.load_committed_blob

        def changed(source: Path, head: str, relative: str, label: str) -> bytes:
            data: bytes = original(source, head, relative, label)
            return data + b"# Changed.\n" if relative.startswith("scripts/lib/") else data

        with patch.object(installer, "load_committed_blob", side_effect=changed):
            with self.assertRaisesRegex(ValueError, "library bytes do not match the preview"):
                installer.install(value, committed, installer.plan_hash(value))
        self.assertFalse((self.state / "PAUSED").exists())
        self.assertFalse((self.state / "releases").exists())

    def test_each_installed_library_hash_is_checked(self) -> None:
        self.real_gateway()
        value, committed = self.plan()
        installer.install(value, committed, installer.plan_hash(value))
        release = self.state / "releases" / installer.release_name(value)
        for relative in library_digests(value):
            with self.subTest(module=relative):
                target = release / relative
                original = target.read_bytes()
                target.chmod(0o644)
                target.write_text("raise RuntimeError('tampered library executed')\n")
                for condition in ("tampered", "missing"):
                    if condition == "missing":
                        target.unlink()
                    for name in ("rebase-clean.sh", "train.sh", "merge_train_operator.py"):
                        with self.subTest(adapter=name, condition=condition):
                            result = self.run_adapter(name, self.directory)
                            self.assertEqual(result.returncode, 1, result.stderr)
                            self.assertIn(
                                f"refused: installed library hash changed: {relative}",
                                result.stderr,
                            )
                            self.assertNotIn("Traceback", result.stderr)
                target.write_bytes(original)
                target.chmod(0o444)

    def test_existing_different_library_refuses_overwrite(self) -> None:
        value, committed = self.plan()
        release = self.state / "releases" / installer.release_name(value)
        target = release / "scripts/lib/__init__.py"
        target.parent.mkdir(parents=True)
        target.write_text("# Foreign evidence.\n")
        with self.assertRaisesRegex(ValueError, "unexpected library evidence"):
            installer.install(value, committed, installer.plan_hash(value))
        self.assertEqual(target.read_text(), "# Foreign evidence.\n")

    def test_library_directory_symlink_refuses_without_following(self) -> None:
        value, committed = self.plan()
        release = self.state / "releases" / installer.release_name(value)
        release.mkdir(parents=True)
        outside = self.directory / "foreign libraries"
        outside.mkdir()
        (release / "scripts").symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "symlink release directory"):
            installer.install(value, committed, installer.plan_hash(value))
        self.assertEqual(list(outside.iterdir()), [])

    def test_library_only_change_creates_new_release_and_preserves_old(self) -> None:
        self.real_gateway()
        value, committed = self.plan()
        installer.install(value, committed, installer.plan_hash(value))
        old = self.state / "releases" / installer.release_name(value)
        before = {str(p.relative_to(old)): p.read_bytes() for p in old.rglob("*.py")}
        relative = "scripts/lib/safe_subprocess.py"
        original = (self.source / relative).read_bytes()
        (self.source / relative).write_bytes(original + b"# Library-only revision.\n")
        installer.git(self.source, "commit", "-am", "library only")
        fresh, fresh_committed = self.plan()
        self.assertEqual(fresh["gateway_sha256"], value["gateway_sha256"])
        self.assertEqual(fresh["composer_sha256"], value["composer_sha256"])
        installer.install(fresh, fresh_committed, installer.plan_hash(fresh))
        new = self.state / "releases" / installer.release_name(fresh)
        self.assertNotEqual(new, old)
        self.assertEqual((new / relative).read_bytes(), original + b"# Library-only revision.\n")
        self.assertEqual(
            {str(p.relative_to(old)): p.read_bytes() for p in old.rglob("*.py")}, before
        )

    def test_preview_is_read_only_and_apply_preserves_exact_originals_holds_and_policy(
        self,
    ) -> None:
        value, committed = self.plan()
        self.assertFalse((self.state / "PAUSED").exists())
        self.assertEqual({path.name for path in self.state.iterdir()}, set(self.before))
        backup = installer.install(value, committed, installer.plan_hash(value))
        for name, data in self.before.items():
            self.assertEqual((backup / name).read_bytes(), data)
        self.assertEqual((self.state / "hold.txt").read_bytes(), self.before["hold.txt"])
        self.assertTrue((self.state / "PAUSED").exists())
        self.assertEqual(
            json.loads((self.state / "policy.json").read_text())["protected_branches"],
            ["active-root", "existing"],
        )
        self.assertTrue((backup / "receipt.json").exists())

    def test_installed_observers_default_read_only_and_gateway_tampering_fails(self) -> None:
        value, committed = self.plan()
        installer.install(value, committed, installer.plan_hash(value))
        for argv in (
            [str(self.state / "train.sh")],
            [sys.executable, str(self.state / "merge_train_operator.py")],
        ):
            output = guard.execute(argv, self.source)
            arguments = json.loads(output)
            self.assertEqual(arguments[0], "cycle")
            self.assertNotIn("--apply", arguments)
        with self.assertRaises(guard.Refused):
            guard.execute(
                [sys.executable, str(self.state / "merge_train_operator.py"), "--apply"],
                self.source,
            )
        with self.assertRaises(guard.Refused):
            guard.execute([str(self.state / "watchdog.sh"), "--apply"], self.source)
        gateway = self.state / "releases" / installer.release_name(value) / "merge_train_guard.py"
        gateway.chmod(0o644)
        gateway.write_text("print('tampered')\n")
        with self.assertRaisesRegex(guard.Refused, "hash changed"):
            guard.execute([str(self.state / "train.sh")], self.source)

    def test_stale_preview_and_source_edits_refuse_before_replacing_runtime(self) -> None:
        """Prove a stale preview makes install refuse and keeps the runtime files.

        An uncommitted gateway or composer edit makes plan refuse with ValueError.
        """
        value, committed = self.plan()
        (self.state / "hold.txt").write_text("1422 new hold\n")
        fresh, _ = self.plan()
        with self.assertRaisesRegex(ValueError, "plan changed"):
            installer.install(fresh, committed, installer.plan_hash(value))
        for name in installer.RUNTIME_FILES:
            self.assertEqual((self.state / name).read_bytes(), self.before[name])
        (self.source / installer.SOURCE_PATH).write_text("print('uncommitted')\n")
        with self.assertRaisesRegex(ValueError, "committed"):
            self.plan()
        installer.git(self.source, "checkout", "--", installer.SOURCE_PATH)
        (self.source / installer.COMPOSER_PATH).write_text("print('uncommitted composer')\n")
        with self.assertRaisesRegex(ValueError, "committed"):
            self.plan()

    def test_target_symlink_is_refused_and_never_followed(self) -> None:
        outside = self.directory / "evidence"
        outside.write_text("keep\n")
        target = self.state / "train.sh"
        target.unlink()
        target.symlink_to(outside)
        with self.assertRaisesRegex(ValueError, "non-regular"):
            self.plan()
        self.assertEqual(outside.read_text(), "keep\n")

    def test_failed_install_leaves_pause_backups_and_foreign_release_untouched(self) -> None:
        value, committed = self.plan()
        outside = self.directory / "foreign-release"
        outside.mkdir()
        (outside / "keep.txt").write_text("unknown evidence\n")
        (self.state / "releases").symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "symlink release"):
            installer.install(value, committed, installer.plan_hash(value))
        self.assertTrue((self.state / "PAUSED").exists())
        self.assertEqual(list(outside.iterdir()), [outside / "keep.txt"])
        backup = next(self.state.glob("migration-*"))
        self.assertTrue((backup / "plan.json").exists())
        for name in installer.RUNTIME_FILES:
            self.assertEqual((backup / name).read_bytes(), self.before[name])
            self.assertEqual((self.state / name).read_bytes(), self.before[name])

    def test_cooperating_mutation_lock_blocks_install(self) -> None:
        value, committed = self.plan()
        train = guard.Train(self.source, self.state)
        with train.mutation_lock(), self.assertRaises(BlockingIOError):
            installer.install(value, committed, installer.plan_hash(value))
        self.assertFalse((self.state / "PAUSED").exists())

    def test_generated_shell_adapters_parse_and_rebase_is_explicit(self) -> None:
        value, committed = self.plan()
        installer.install(value, committed, installer.plan_hash(value))
        for name in ("train.sh", "watchdog.sh", "rebase-clean.sh"):
            guard.execute(["sh", "-n", str(self.state / name)], self.source)
        arguments = json.loads(
            guard.execute([str(self.state / "rebase-clean.sh"), "1422", "--apply"], self.source)
        )
        self.assertEqual(arguments[:3], ["rebase", "1422", "--apply"])

    def test_installed_composer_recorded_sha_mode_and_loadable(self) -> None:
        """Prove install writes read-only composer matching recorded hash loadable by gateway."""
        value, committed = self.plan()
        installer.install(value, committed, installer.plan_hash(value))
        release = self.state / "releases" / installer.release_name(value)
        composer = release / "squash_message.py"
        self.assertTrue(composer.exists())
        self.assertEqual(installer.sha256(composer.read_bytes()), value["composer_sha256"])
        self.assertEqual(stat.S_IMODE(composer.stat().st_mode), 0o444)
        spec = importlib.util.spec_from_file_location(
            "installed_gateway", str(release / "merge_train_guard.py")
        )
        self.assertIsNotNone(spec)
        assert spec is not None and spec.loader is not None
        installed_gateway = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(installed_gateway)
        loaded_composer = installed_gateway.load_squash_composer()
        self.assertTrue(getattr(loaded_composer, "TEST_COMPOSER", False))

    def test_installed_composer_tampering_refuses_launcher(self) -> None:
        """Prove modified installed composer causes launcher execution to refuse with Refused."""
        value, committed = self.plan()
        installer.install(value, committed, installer.plan_hash(value))
        composer = self.state / "releases" / installer.release_name(value) / "squash_message.py"
        composer.chmod(0o644)
        composer.write_text("print('tampered composer')\n")
        with self.assertRaisesRegex(guard.Refused, "refused: installed composer hash changed"):
            guard.execute([str(self.state / "train.sh")], self.source)

    def test_existing_release_with_different_composer_refuses_and_preserves_old(self) -> None:
        """Prove existing differing composer in release dir raises ValueError and preserves file."""
        value, committed = self.plan()
        release = self.state / "releases" / installer.release_name(value)
        release.mkdir(parents=True)
        composer = release / "squash_message.py"
        composer.write_text("EXISTING_UNTOUCHED = True\n")
        with self.assertRaisesRegex(ValueError, "unexpected composer evidence"):
            installer.install(value, committed, installer.plan_hash(value))
        self.assertEqual(composer.read_text(), "EXISTING_UNTOUCHED = True\n")

    def test_composer_only_change_installs_into_a_new_release(self) -> None:
        """Prove a composer-only commit installs beside the old release and leaves it intact."""
        value, committed = self.plan()
        installer.install(value, committed, installer.plan_hash(value))
        old = self.state / "releases" / installer.release_name(value)
        (self.source / installer.COMPOSER_PATH).write_text("TEST_COMPOSER = 2\n")
        installer.git(self.source, "commit", "-am", "composer only")
        fresh, fresh_committed = self.plan()
        self.assertEqual(fresh["gateway_sha256"], value["gateway_sha256"])
        installer.install(fresh, fresh_committed, installer.plan_hash(fresh))
        new = self.state / "releases" / installer.release_name(fresh)
        self.assertNotEqual(new, old)
        self.assertEqual((new / "squash_message.py").read_text(), "TEST_COMPOSER = 2\n")
        self.assertEqual((old / "squash_message.py").read_text(), "TEST_COMPOSER = True\n")

    def test_installer_git_ignores_the_callers_global_config(self) -> None:
        """Prove installer.git ignores the caller's global Git configuration."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            (tmp / ".gitconfig").write_text("[planted]\n\tkey = leaked\n")
            with patch.dict(os.environ, {"HOME": str(tmp), "XDG_CONFIG_HOME": str(tmp)}):
                with self.assertRaises(CommandFailed):
                    installer.git(self.source, "config", "--get", "planted.key")


if __name__ == "__main__":
    unittest.main()
