# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Execute the configured lint driver and Make target in scratch Git repos."""

from __future__ import annotations

import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from collections.abc import Sequence
from pathlib import Path
from typing import Any

SCRIPT = Path(__file__).resolve().parents[1] / "lint-configured.py"
ROOT = SCRIPT.parents[2]
PUBLIC_MODEL = Path("scripts/ci/cppcheck-public-entrypoints.cfg")


class PublicEntrypointModelTests(unittest.TestCase):
    def validate_model(self, root: Path) -> set[str]:
        """Validate the checked-in subset against this repo's explicit header lists."""
        model = ET.fromstring(  # noqa: S314 -- checked-in model or local fixture XML
            (root / PUBLIC_MODEL).read_text(encoding="utf-8")
        )
        self.assertEqual((model.tag, model.attrib), ("def", {"format": "2"}))
        self.assertTrue(len(model), "public entrypoint model must not be empty")
        self.assertFalse((model.text or "").strip())
        names: set[str] = set()
        for node in model:
            self.assertEqual(node.tag, "entrypoint")
            self.assertEqual(set(node.attrib), {"name"})
            name = node.attrib["name"]
            self.assertRegex(name, r"\Avmaf_[a-z0-9_]+\Z")
            self.assertNotIn(name, names, "duplicate public entrypoint")
            self.assertFalse(len(node), "entrypoints cannot contain nested policy")
            self.assertFalse((node.text or "").strip())
            self.assertFalse((node.tail or "").strip())
            names.add(name)

        include = root / "core/include/libvmaf"
        # These are the explicit lists used by this Meson file, not a general
        # Meson evaluator. A new declaration outside these lists fails closed.
        meson = re.sub(r"#.*", "", (include / "meson.build").read_text(encoding="utf-8"))
        installed = re.search(r"install_headers\(\s*\[([^\]]+)\]([^)]*)\)", meson, re.DOTALL)
        self.assertIsNotNone(installed, "expected explicit installed-header list")
        assert installed is not None
        headers = set(re.findall(r"'([^']+)'", installed[1]))
        conditional = re.findall(r"platform_specific_headers\s*\+=\s*'([^']+)'", meson)
        if conditional:
            self.assertIn(
                "platform_specific_headers",
                [argument.strip() for argument in installed[2].split(",")],
                "conditional public-header list must reach install_headers",
            )
        headers.update(conditional)
        declarations: dict[str, list[str]] = {}
        for header in sorted(headers):
            self.assertEqual(Path(header).name, header, "public header must be a basename")
            text = (include / header).read_text(encoding="utf-8")
            text = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.DOTALL)
            for name in re.findall(
                r"\bVMAF_EXPORT\s+[^;{}]*?\b(vmaf_[a-z0-9_]+)\s*\([^;{}]*\)\s*;", text
            ):
                declarations.setdefault(name, []).append(header)
        for name in names:
            self.assertEqual(
                len(declarations.get(name, [])),
                1,
                f"{name}: expected one VMAF_EXPORT declaration in an installed header",
            )
        return names

    def test_shipped_model_has_only_declared_public_roots(self) -> None:
        self.validate_model(ROOT)

    def test_missing_invalid_and_non_public_entries_fail_validation(self) -> None:
        with tempfile.TemporaryDirectory(prefix="cppcheck-public-model-") as temporary:
            root = Path(temporary)
            model = root / PUBLIC_MODEL
            model.parent.mkdir(parents=True)
            include = root / "core/include/libvmaf"
            include.mkdir(parents=True)
            (include / "meson.build").write_text("install_headers(['public.h'])\n")
            (include / "public.h").write_text(
                "VMAF_EXPORT int vmaf_public(void);\nint vmaf_private(void);\n"
                "/* VMAF_EXPORT int vmaf_comment(void); */\n"
            )
            (include / "not_installed.h").write_text("VMAF_EXPORT int vmaf_uninstalled(void);\n")
            with self.assertRaises(FileNotFoundError):
                self.validate_model(root)
            for body in (
                "",
                "<entrypoint/>",
                '<entrypoint name="vmaf_private"/>',
                '<entrypoint name="vmaf_comment"/>',
                '<entrypoint name="vmaf_uninstalled"/>',
                '<entrypoint name="vmaf_misspelled"/>',
                '<entrypoint name="vmaf_*"/>',
                '<entrypoint name="vmaf_public"/>' * 2,
                '<function name="vmaf_public"/>',
            ):
                with self.subTest(body=body):
                    model.write_text(f'<def format="2">{body}</def>')
                    with self.assertRaises(AssertionError):
                        self.validate_model(root)
            model.write_text('<def format="2"><entrypoint name="vmaf_public"/></def>')
            self.assertEqual(self.validate_model(root), {"vmaf_public"})

    def test_conditional_header_list_must_be_consumed_by_install(self) -> None:
        with tempfile.TemporaryDirectory(prefix="cppcheck-public-install-") as temporary:
            root = Path(temporary)
            model = root / PUBLIC_MODEL
            model.parent.mkdir(parents=True)
            model.write_text('<def format="2"><entrypoint name="vmaf_conditional"/></def>')
            include = root / "core/include/libvmaf"
            include.mkdir(parents=True)
            (include / "public.h").write_text("/* unconditional public header */\n")
            (include / "conditional.h").write_text("VMAF_EXPORT int vmaf_conditional(void);\n")
            for consumed in (True, False):
                with self.subTest(consumed=consumed):
                    argument = ", platform_specific_headers" if consumed else ""
                    (include / "meson.build").write_text(
                        "platform_specific_headers = []\n"
                        "if enabled\n  platform_specific_headers += 'conditional.h'\nendif\n"
                        f"install_headers(['public.h']{argument})\n"
                    )
                    if consumed:
                        self.assertEqual(self.validate_model(root), {"vmaf_conditional"})
                    else:
                        with self.assertRaisesRegex(AssertionError, "must reach install_headers"):
                            self.validate_model(root)

    def test_model_declaration_workflow_and_control_changes_route_to_hook(self) -> None:
        config = (ROOT / ".pre-commit-config.yaml").read_text()
        hook = config.split("- id: test-configured-lint-driver", 1)[1].split("- id:", 1)[0]
        pattern = re.search(r"files: '([^']+)'", hook)
        self.assertIsNotNone(pattern)
        assert pattern is not None
        for path in (
            str(PUBLIC_MODEL),
            "scripts/ci/lint-configured.py",
            "scripts/ci/tests/test_lint_configured.py",
            "scripts/ci/tests/test_cppcheck_posix_model.py",
            "core/include/libvmaf/libvmaf_hip.h",
            "core/include/libvmaf/meson.build",
            ".github/workflows/lint-and-format.yml",
            ".pre-commit-config.yaml",
        ):
            with self.subTest(path=path):
                self.assertRegex(path, pattern[1])
        self.assertIn("-p test_lint_configured.py", hook)


class ConfiguredLintTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="configured lint ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.build = self.root / "configured-build"
        self.build.mkdir()
        self.database = self.build / "compile_commands.json"
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.calls = self.root / "calls"
        self.calls.mkdir()
        self.env = dict(
            {key: value for key, value in os.environ.items() if not key.startswith("GIT_")},
            PATH=f"{self.bin}{os.pathsep}{os.environ['PATH']}",
            CALLS=str(self.calls),
            GIT_CONFIG_NOSYSTEM="1",
            GIT_CONFIG_GLOBAL=os.devnull,
        )
        self.native = [
            "core/src/engine.c",
            "core/src/dict.cpp",
            "core/test/test_engine.c",
            "core/tools/vmaf.cpp",
            "core/src/feature/cuda/active.cu",
            "core/src/vendor/tracked.c",
        ]
        for name in [*self.native, "core/src/feature/arm64/inactive.c"]:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("int fixture;\n", encoding="utf-8")
        (self.root / ".cppcheck-suppressions.txt").write_text("", encoding="utf-8")
        (self.root / PUBLIC_MODEL).parent.mkdir(parents=True)
        shutil.copy2(ROOT / PUBLIC_MODEL, self.root / PUBLIC_MODEL)
        self.command(["git", "init", "-q"])
        self.command(["git", "add", "."])
        self.entries = [self.entry(name) for name in self.native]
        self.entries += [self.entry(self.native[0], ["-DMODE=two", "-flto=4"])]
        # Generated and external inputs are not tracked project source.
        self.entries += [
            {
                "directory": str(self.build),
                "file": "generated.c",
                "arguments": ["cc", "-c", "generated.c"],
            }
        ]
        self.write_database()
        for name in ["clang-tidy", "cppcheck"]:
            stub = self.bin / name
            stub.write_text(
                f"#!{sys.executable}\n"
                "import json, os, sys\n"
                "from pathlib import Path\n"
                "name = Path(sys.argv[0]).name\n"
                "Path(os.environ['CALLS'], name + '-' + str(os.getpid()) + '.json').write_text(json.dumps(sys.argv[1:]))\n"
                "raise SystemExit(int(os.environ.get(name.upper().replace('-', '_') + '_EXIT', '0')))\n",
                encoding="utf-8",
            )
            stub.chmod(0o755)

    def command(self, argv: list[str], **kwargs: Any) -> subprocess.CompletedProcess[str]:
        result = subprocess.run(  # noqa: S603 -- fixture-owned argv, no shell
            argv, cwd=self.root, env=self.env, capture_output=True, text=True, check=False, **kwargs
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def entry(self, name: str, flags: Sequence[str] = ()) -> dict[str, Any]:
        return {
            "directory": str(self.build),
            "file": str(self.root / name),
            "arguments": ["gcc", *flags, "-c", str(self.root / name)],
        }

    def write_database(self) -> None:
        self.database.write_text(json.dumps(self.entries), encoding="utf-8")

    def run_driver(self, **env: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(  # noqa: S603 -- execute fixture driver without shell
            [
                sys.executable,
                str(SCRIPT),
                "--repo-root",
                str(self.root),
                "--build-dir",
                str(self.build),
                "--jobs",
                "2",
            ],
            cwd=self.root,
            env={**self.env, **env},
            capture_output=True,
            text=True,
            check=False,
        )

    def report(self) -> Path:
        return next(self.build.glob("lint-configured-*/scope.json")).parent

    def test_configured_scope_variants_and_original_database(self) -> None:
        original = self.database.read_bytes()
        result = self.run_driver()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.database.read_bytes(), original)
        report = self.report()
        scope = json.loads((report / "scope.json").read_text())
        self.assertEqual(set(scope["configured_sources"]), set(self.native))
        self.assertEqual(scope["configured_command_count"], len(self.native) + 1)
        self.assertEqual(
            scope["unconfigured_tracked_sources"], ["core/src/feature/arm64/inactive.c"]
        )
        self.assertEqual(len(scope["lto_adaptations"]), 1)
        entries = json.loads((report / "compile_commands.json").read_text())
        variants = [
            entry["arguments"]
            for entry in entries
            if entry["file"].endswith("engine.c") and "/src/" in entry["file"]
        ]
        self.assertEqual(len(variants), 2)
        self.assertIn("-DMODE=two", variants[1])
        self.assertIn("-flto", variants[1])
        calls = [json.loads(path.read_text()) for path in self.calls.glob("clang-tidy-*.json")]
        self.assertEqual(
            {args[-1] for args in calls}, {str(self.root / name) for name in self.native}
        )
        self.assertEqual(len(calls), len(self.native))
        cppcheck_calls = [
            json.loads(path.read_text()) for path in self.calls.glob("cppcheck-*.json")
        ]
        self.assertEqual(len(cppcheck_calls), 1)
        self.assertEqual(
            cppcheck_calls[0],
            [
                "--enable=all",
                "--check-level=exhaustive",
                "--inline-suppr",
                "--library=posix",
                f"--library={self.root / PUBLIC_MODEL}",
                f"--suppressions-list={self.root / '.cppcheck-suppressions.txt'}",
                f"--project={report / 'compile_commands.json'}",
                "--error-exitcode=1",
            ],
        )

    def test_command_form_and_only_numeric_lto_adaptation(self) -> None:
        flags = [
            "-flto=thin",
            "-flto=full",
            "-flto=invalid",
            "-flto=0",
            "-flto=auto",
            "-flto=jobserver",
            "-flto=12",
        ]
        self.entries = [
            {
                "directory": str(self.build),
                "file": str(self.root / self.native[0]),
                "command": shlex.join(["gcc", *flags, "-c", str(self.root / self.native[0])]),
            }
        ]
        self.write_database()
        result = self.run_driver()
        self.assertEqual(result.returncode, 0, result.stderr)
        entry = json.loads((self.report() / "compile_commands.json").read_text())[0]
        self.assertEqual(entry["arguments"][1:-2], [*flags[:-1], "-flto"])

    def test_posix_model_preserves_windows_and_cpp_command_settings(self) -> None:
        flags = ["-D_WIN32", "-D_WIN64", "--target=x86_64-w64-mingw32", "-std=c++23"]
        self.entries = [self.entry("core/src/dict.cpp", flags)]
        self.write_database()
        result = self.run_driver()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        entries = json.loads((self.report() / "compile_commands.json").read_text())
        self.assertEqual(entries[0]["arguments"], self.entries[0]["arguments"])
        args = json.loads(next(self.calls.glob("cppcheck-*.json")).read_text())
        self.assertIn("--library=posix", args)
        self.assertIn(f"--library={self.root / PUBLIC_MODEL}", args)
        self.assertIn("--check-level=exhaustive", args)
        self.assertFalse(any(arg.startswith(("--platform", "--language", "--std")) for arg in args))

    def test_ci_runs_real_model_controls_and_retains_diagnostic_categories(self) -> None:
        workflow = (ROOT / ".github/workflows/lint-and-format.yml").read_text()
        job = workflow.split("\n  cppcheck:\n", 1)[1].split("\n  python-lint:", 1)[0]
        self.assertIn("-p test_cppcheck_posix_model.py", job)
        self.assertIn("--library=posix", job)
        self.assertIn(f"--library={PUBLIC_MODEL.as_posix()}", job)
        self.assertIn("--check-level=exhaustive", job)
        self.assertNotIn("--check-level=normal", job)
        self.assertNotIn("--check-level=reduced", job)
        self.assertIn("--enable=warning,performance,portability", job)
        self.assertIn("--error-exitcode=1", job)
        self.assertIn("--project=build/compile_commands.json", job)
        self.assertNotIn("--disable", job)

    def test_both_analyzers_run_and_failures_propagate(self) -> None:
        for failing in ["CLANG_TIDY_EXIT", "CPPCHECK_EXIT"]:
            with self.subTest(failing=failing):
                result = self.run_driver(**{failing: "7"})
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(len(list(self.calls.glob("cppcheck-*.json"))), 2)

    def test_missing_empty_and_invalid_database_fail_before_analyzers(self) -> None:
        cases = [None, "[]", "{}", "invalid", "[null]"]
        for raw in cases:
            with self.subTest(raw=raw):
                if raw is None:
                    self.database.unlink()
                else:
                    self.database.write_text(raw, encoding="utf-8")
                result = self.run_driver()
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertIn("configured lint:", result.stderr)
        self.assertEqual(list(self.calls.iterdir()), [])

    def test_missing_tracked_file_and_invalid_commands_fail(self) -> None:
        for argv in [None, [], [42], [""], "gcc -c file"]:
            with self.subTest(argv=argv):
                self.entries = [self.entry(self.native[0])]
                self.entries[0]["arguments"] = argv
                self.write_database()
                self.assertEqual(self.run_driver().returncode, 2)
        self.entries = [self.entry(self.native[0])]
        self.write_database()
        (self.root / self.native[0]).unlink()
        result = self.run_driver()
        self.assertEqual(result.returncode, 2)
        self.assertIn("configured tracked source is missing", result.stderr)
        self.assertEqual(list(self.calls.iterdir()), [])

    def test_relative_source_paths_keep_compile_directory(self) -> None:
        self.entries = [self.entry(self.native[0])]
        self.entries[0]["file"] = "../core/src/engine.c"
        self.write_database()
        result = self.run_driver()
        self.assertEqual(result.returncode, 0, result.stderr)
        entry = json.loads((self.report() / "compile_commands.json").read_text())[0]
        self.assertEqual(entry["file"], str(self.root / self.native[0]))
        self.assertEqual(entry["directory"], str(self.build))

    def test_no_selected_sources_and_malformed_entry_fail(self) -> None:
        examples: list[list[dict[str, Any]]] = [
            [
                {
                    "directory": str(self.build),
                    "file": "generated.c",
                    "arguments": ["cc", "-c", "generated.c"],
                }
            ],
            [
                {
                    "directory": "relative",
                    "file": str(self.root / self.native[0]),
                    "command": "cc -c file",
                }
            ],
            [
                {
                    "directory": str(self.build),
                    "file": str(self.root / self.native[0]),
                    "command": "cc 'unterminated",
                }
            ],
            [{"directory": str(self.build), "file": str(self.root / self.native[0])}],
        ]
        for entries in examples:
            with self.subTest(entries=entries):
                self.entries = entries
                self.write_database()
                result = self.run_driver()
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertEqual(list(self.calls.iterdir()), [])

    def test_inherited_git_environment_cannot_redirect_selection(self) -> None:
        foreign = self.root / "foreign"
        foreign.mkdir()
        self.command(["git", "-C", str(foreign), "init", "-q"])
        result = self.run_driver(
            GIT_DIR=str(foreign / ".git"),
            GIT_WORK_TREE=str(foreign),
            GIT_INDEX_FILE=str(foreign / ".git/index"),
            GIT_CONFIG_COUNT="1",
            GIT_CONFIG_KEY_0="core.worktree",
            GIT_CONFIG_VALUE_0=str(foreign),
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        scope = json.loads((self.report() / "scope.json").read_text())
        self.assertEqual(set(scope["configured_sources"]), set(self.native))

    def test_untracked_entry_with_invalid_command_schema_fails(self) -> None:
        self.entries += [{"directory": str(self.build), "file": "untracked.c", "arguments": [42]}]
        self.write_database()
        result = self.run_driver()
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("invalid arguments", result.stderr)
        self.assertEqual(list(self.calls.iterdir()), [])

    def test_analyzer_launch_failure_still_runs_other_analyzer(self) -> None:
        (self.bin / "clang-tidy").write_text(
            "#!/nonexistent/vmafx-lint-fixture\n", encoding="utf-8"
        )
        result = self.run_driver()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("Cannot execute analyzer", result.stdout)
        self.assertEqual(len(list(self.calls.glob("cppcheck-*.json"))), 1)
        receipt = json.loads((self.report() / "result.json").read_text())
        self.assertEqual(len(receipt["clang_tidy_failed_sources"]), len(self.native))
        self.assertEqual(receipt["cppcheck_exit"], 0)

    def test_non_utf8_diagnostics_preserve_failure_and_other_analyzer(self) -> None:
        (self.bin / "clang-tidy").write_text(
            f"#!{sys.executable}\nimport os\nos.write(1, bytes([255]))\nraise SystemExit(1)\n",
            encoding="utf-8",
        )
        result = self.run_driver()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(len(list(self.calls.glob("cppcheck-*.json"))), 1)
        self.assertTrue((self.report() / "result.json").is_file())
        self.assertIn(bytes([255]), (self.report() / "clang-tidy-0000.log").read_bytes())

    def test_output_distinguishes_preserved_modes_and_requires_string(self) -> None:
        self.entries = [self.entry(self.native[0]), self.entry(self.native[0])]
        self.entries[0]["output"] = "library-mode.o"
        self.entries[1]["output"] = "test-mode.o"
        self.write_database()
        result = self.run_driver()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        entries = json.loads((self.report() / "compile_commands.json").read_text())
        self.assertEqual([entry["output"] for entry in entries], ["library-mode.o", "test-mode.o"])
        self.entries[0]["output"] = 42
        self.write_database()
        result = self.run_driver()
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("invalid output", result.stderr)

    def run_make_target(self, *, polluted: bool = False) -> None:
        shutil.copy2(ROOT / "Makefile", self.root / "Makefile")
        target = self.root / "scripts/ci/lint-configured.py"
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(SCRIPT, target)
        original = self.database.read_bytes()
        (self.root / "native-database.json").write_bytes(original)
        if polluted:
            self.entries += [
                {
                    "directory": str(self.build),
                    "file": "meson-internal__test",
                    "command": "",
                    "output": "test",
                }
            ]
            self.write_database()
        meson = self.bin / "meson"
        meson.write_text(
            f"#!{sys.executable}\n"
            "import os, sys\nfrom pathlib import Path\n"
            "assert sys.argv[1:] == ['setup', '--reconfigure', 'configured-build', 'core']\n"
            "assert os.environ['PATH'].split(os.pathsep)[0] == 'fixture-venv/bin'\n"
            "Path('configured-build/compile_commands.json').write_bytes(Path('native-database.json').read_bytes())\n"
            "Path('configured-build/configure-called.txt').write_text('same build options')\n",
            encoding="utf-8",
        )
        meson.chmod(0o755)
        ninja = self.bin / "ninja"
        ninja.write_text(
            f"#!{sys.executable}\n"
            "import json, sys\n"
            "from pathlib import Path\n"
            "if '-t' in sys.argv:\n"
            "    print(json.dumps([{'directory': str(Path.cwd()), 'file': 'generator.txt', 'arguments': ['generator']}]))\n"
            "else:\n"
            "    assert Path('configured-build/configure-called.txt').is_file()\n"
            "    Path('configured-build/build-called.txt').write_text('generated prerequisites ready')\n",
            encoding="utf-8",
        )
        ninja.chmod(0o755)
        # Mark existing tools old: this fixture must never bootstrap Meson/Ninja.
        result = self.command(
            [
                "make",
                "--no-print-directory",
                "-o",
                "bin/meson",
                "-o",
                "bin/ninja",
                "lint-c",
                f"PYTHON_INTERPRETER={sys.executable}",
                "MESON=bin/meson",
                "NINJA=bin/ninja",
                "BUILD_DIR=configured-build",
                "VENV=fixture-venv",
                "LINT_JOBS=2",
            ]
        )
        self.assertEqual(self.database.read_bytes(), original)
        self.assertTrue((self.build / "build-called.txt").is_file())
        self.assertIn("Configured lint:", result.stdout)
        self.assertEqual(len(list(self.calls.glob("clang-tidy-*.json"))), len(self.native))

    def test_real_make_target_uses_native_database(self) -> None:
        self.run_make_target()

    def test_real_make_repairs_old_phony_database_after_noop_build(self) -> None:
        self.run_make_target(polluted=True)


if __name__ == "__main__":
    unittest.main()
