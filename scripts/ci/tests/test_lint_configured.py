# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Execute the configured lint driver and Make target in scratch Git repos."""

from __future__ import annotations

import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from collections.abc import Sequence
from pathlib import Path
from typing import Any

SCRIPT = Path(__file__).resolve().parents[1] / "lint-configured.py"
ROOT = SCRIPT.parents[2]


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
        self.assertEqual(len(list(self.calls.glob("cppcheck-*.json"))), 1)

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
        target.parent.mkdir(parents=True)
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
