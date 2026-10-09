#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The changed-files clang-tidy routing defers only to a lane that measures the file.

Planted trees exercise both modes of scripts/ci/tidy_changed_route.py: a file
no lane measures fails the Tidy Changed job and is named; a file a lane
measures is skipped with its lane named; a header reached only through units
of other lanes follows them; a C-only header is linted through its C includers
in the Tidy SYCL job (Q-341, Q-342).
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "tidy_changed_route.py"
GIT = shutil.which("git") or "git"


def clean_environment() -> dict[str, str]:
    """The environment without GIT_* variables. A git hook exports GIT_INDEX_FILE
    (absolute in a linked worktree) and GIT_DIR; inherited, they would make the
    fixture's `git add -A` rewrite the index of the repository running the hook."""
    return {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}


FILES = {
    "core/src/lib.c": '#include "lib.h"\n',
    "core/src/lib.h": "int lib(void);\n",
    "core/src/orphan.h": "int orphan(void);\n",
    "core/test/test_cpu.c": '#include "util.h"\n',
    "core/test/util.h": "int util(void);\n",
    "core/test/exact_cells.h": "int exact(void);\n",
    "core/test/device_cells.h": '#include "exact_cells.h"\n',
    "core/test/test_import_cuda.c": '#include "device_cells.h"\n',
    "core/test/test_import_sycl.c": '#include "../src/sycl/internal.h"\n',
    "core/test/test_unmeasured_gpu.c": '#include "lonely.h"\n',
    "core/test/lonely.h": "int lonely(void);\n",
    "core/src/sycl/import.c": '#include "internal.h"\n',
    "core/src/sycl/internal.h": "int internal(void);\n",
    "core/src/sycl/shared.h": "int shared(void);\n",
    "core/src/sycl/runtime.cpp": '#include "shared.h"\n',
    "core/src/sycl/c_only_unbuilt.h": "int unbuilt(void);\n",
    "core/src/sycl/unbuilt.c": '#include "c_only_unbuilt.h"\n',
    "core/test/test_excepted.c": "int excepted;\n",
    "core/test/test_expired.c": "int expired;\n",
}
EXCEPTIONS = """[[exception]]
path = "core/test/test_excepted.c"
reason = "Planted: built only with an option no lane configures."
expires = 2027-01-31

[[exception]]
path = "core/test/test_expired.c"
reason = "Planted: an entry past its expiry."
expires = 2026-01-31
"""
CPU_COMMANDS = ("core/src/lib.c", "core/test/test_cpu.c")
SYCL_COMMANDS = (
    "core/src/sycl/import.c",
    "core/test/test_import_sycl.c",
    "core/src/sycl/runtime.cpp",
)
BASELINES = {
    "cpu": ["core/src/lib.c", "core/test/test_cpu.c"],
    "cuda": ["core/test/test_import_cuda.c"],
    "hip": ["core/test/test_import_cuda.c"],
    "sycl": ["core/src/sycl/import.c", "core/test/test_import_sycl.c"],
}


class RoutedTree(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        for path, text in FILES.items():
            (self.root / path).parent.mkdir(parents=True, exist_ok=True)
            (self.root / path).write_text(text, encoding="utf-8")
        (self.root / "scripts/ci").mkdir(parents=True)
        for lane, sources in BASELINES.items():
            baseline = self.root / f"scripts/ci/tidy-baseline-{lane}.json"
            baseline.write_text(json.dumps({"measured_sources": sources}), encoding="utf-8")
        exceptions = self.root / ".config/lint-exceptions.d/clang-tidy-coverage.toml"
        exceptions.parent.mkdir(parents=True)
        exceptions.write_text(EXCEPTIONS, encoding="utf-8")
        for argv in ([GIT, "init", "-q", str(self.root)], [GIT, "-C", str(self.root), "add", "-A"]):
            subprocess.run(  # noqa: S603 -- resolved git, fixed argv
                argv, check=True, timeout=60, env=clean_environment()
            )
        self.cpu_db = self.database("cpu.json", CPU_COMMANDS)
        self.sycl_db = self.database("sycl.json", SYCL_COMMANDS)

    def database(self, name: str, files: tuple[str, ...]) -> Path:
        path = self.root / name
        entries = [{"directory": str(self.root), "file": f, "command": f"cc -c {f}"} for f in files]
        path.write_text(json.dumps(entries), encoding="utf-8")
        return path

    def route(self, mode: str, database: Path, *paths: str) -> subprocess.CompletedProcess[str]:
        argv = [sys.executable, str(SCRIPT), mode, "--db", str(database), "--root", str(self.root)]
        environment = {**clean_environment(), "LINT_EXCEPTIONS_TODAY": "2026-10-09"}
        return subprocess.run(  # noqa: S603 -- this test's own script, fixed argv
            argv,
            input="\n".join(paths) + "\n",
            capture_output=True,
            text=True,
            timeout=60,
            env=environment,
        )


class HookEnvironment(unittest.TestCase):
    def test_inherited_git_index_is_never_written(self) -> None:
        # A hook (pre-commit, lefthook) runs this test with GIT_INDEX_FILE set.
        with tempfile.TemporaryDirectory() as raw:
            probe = Path(raw)
            env = clean_environment()
            subprocess.run(  # noqa: S603 -- resolved git, fixed argv
                [GIT, "init", "-q", str(probe)], check=True, timeout=60, env=env
            )
            (probe / "keep.txt").write_text("keep\n", encoding="utf-8")
            subprocess.run(  # noqa: S603 -- resolved git, fixed argv
                [GIT, "-C", str(probe), "add", "keep.txt"], check=True, timeout=60, env=env
            )
            saved = os.environ.get("GIT_INDEX_FILE")
            os.environ["GIT_INDEX_FILE"] = str(probe / ".git/index")
            try:
                tree = RoutedTree("run")
                tree.setUp()
                tree.doCleanups()
            finally:
                if saved is None:
                    del os.environ["GIT_INDEX_FILE"]
                else:
                    os.environ["GIT_INDEX_FILE"] = saved
            listed = subprocess.run(  # noqa: S603 -- resolved git, fixed argv
                [GIT, "-C", str(probe), "ls-files"],
                capture_output=True,
                text=True,
                check=True,
                timeout=60,
                env=env,
            ).stdout.split()
        self.assertEqual(listed, ["keep.txt"])


class ChangedMode(RoutedTree):
    def test_file_with_a_command_is_linted(self) -> None:
        done = self.route("changed", self.cpu_db, "core/src/lib.c")
        self.assertEqual(
            (done.returncode, done.stdout.split()), (0, ["core/src/lib.c"]), done.stderr
        )

    def test_file_a_lane_measures_is_skipped_with_its_lane(self) -> None:
        done = self.route("changed", self.cpu_db, "core/test/test_import_sycl.c")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(done.stdout, "")
        self.assertIn(
            "skip core/test/test_import_sycl.c: measured by the sycl lane(s)", done.stderr
        )

    def test_file_no_lane_measures_fails_and_is_named(self) -> None:
        done = self.route(
            "changed", self.cpu_db, "core/src/lib.c", "core/test/test_unmeasured_gpu.c"
        )
        self.assertEqual(done.returncode, 1)
        self.assertEqual(done.stdout.split(), ["core/src/lib.c"])
        self.assertIn("core/test/test_unmeasured_gpu.c has no compile command", done.stderr)

    def test_header_reached_only_through_other_lanes_follows_them(self) -> None:
        done = self.route(
            "changed", self.cpu_db, "core/test/exact_cells.h", "core/test/device_cells.h"
        )
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(done.stdout, "")
        self.assertIn(
            "skip core/test/exact_cells.h: measured by the cuda, hip lane(s)", done.stderr
        )
        self.assertIn(
            "skip core/test/device_cells.h: measured by the cuda, hip lane(s)", done.stderr
        )

    def test_header_of_an_unmeasured_unit_fails(self) -> None:
        done = self.route("changed", self.cpu_db, "core/test/lonely.h")
        self.assertEqual(done.returncode, 1)
        self.assertIn("core/test/lonely.h has no compile command", done.stderr)

    def test_header_of_a_built_unit_and_an_orphan_header_are_linted(self) -> None:
        done = self.route("changed", self.cpu_db, "core/test/util.h", "core/src/orphan.h")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(done.stdout.split(), ["core/test/util.h", "core/src/orphan.h"])

    def test_file_with_a_live_coverage_exception_is_skipped_naming_it(self) -> None:
        done = self.route("changed", self.cpu_db, "core/test/test_excepted.c")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn(
            "skip core/test/test_excepted.c: no lane reads it; declared clang-tidy-coverage "
            "exception until 2027-01-31",
            done.stderr,
        )

    def test_file_with_an_expired_coverage_exception_fails(self) -> None:
        done = self.route("changed", self.cpu_db, "core/test/test_expired.c")
        self.assertEqual(done.returncode, 1)
        self.assertIn("core/test/test_expired.c has no compile command", done.stderr)


class SyclMode(RoutedTree):
    def test_c_only_header_is_linted_through_its_c_includers(self) -> None:
        done = self.route("sycl", self.sycl_db, "core/src/sycl/internal.h")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(
            done.stdout.split(), ["core/src/sycl/import.c", "core/test/test_import_sycl.c"]
        )
        self.assertIn("core/src/sycl/internal.h is C-only: linted through", done.stderr)

    def test_each_unit_is_linted_once(self) -> None:
        done = self.route(
            "sycl", self.sycl_db, "core/src/sycl/internal.h", "core/src/sycl/import.c"
        )
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(
            done.stdout.split(), ["core/src/sycl/import.c", "core/test/test_import_sycl.c"]
        )

    def test_header_a_cxx_source_includes_is_passed_through(self) -> None:
        done = self.route(
            "sycl", self.sycl_db, "core/src/sycl/shared.h", "core/src/sycl/runtime.cpp"
        )
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(
            done.stdout.split(), ["core/src/sycl/shared.h", "core/src/sycl/runtime.cpp"]
        )

    def test_c_only_header_without_a_built_includer_fails(self) -> None:
        done = self.route("sycl", self.sycl_db, "core/src/sycl/c_only_unbuilt.h")
        self.assertEqual(done.returncode, 1)
        self.assertIn("c_only_unbuilt.h is C-only and no including unit has a command", done.stderr)


if __name__ == "__main__":
    unittest.main()
