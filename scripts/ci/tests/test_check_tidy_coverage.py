#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""scripts/ci/check-tidy-coverage.py fails on a unit no lane reads.

Each case builds a throwaway git repository: baselines that measure some units,
an exception list, and tracked units. The planted defect is a new ``.c`` file
outside every lane and outside the exception list; the check must refuse it.
"""

from __future__ import annotations

import datetime
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import tomllib

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "scripts/ci/check-tidy-coverage.py"
TODAY = "2026-10-05"
LIST = ".config/lint-exceptions.d/clang-tidy-coverage.toml"
# A commit hook runs with GIT_INDEX_FILE and GIT_DIR set: a throwaway repository must not inherit
# them, or `git add -A` below rewrites the index of the repository being committed.
GIT = shutil.which("git") or "git"
CLEAN_ENV = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}


def entry(
    path: str, expires: str = "2027-01-01", reason: str = "no tool reads it"
) -> dict[str, str]:
    return {"path": path, "reason": reason, "expires": expires}


def toml_of(entries: list[dict[str, str]]) -> str:
    """The rule file; ``expires`` is a TOML date, written bare unless the case breaks it."""
    blocks = []
    for item in entries:
        lines = ["[[exception]]"]
        for key, value in item.items():
            bare = key == "expires" and value[:1].isdigit()
            lines.append(f"{key} = {value if bare else json.dumps(value)}")
        blocks.append("\n".join(lines))
    return "\n\n".join(blocks) + "\n"


class Fixture:
    def __init__(self, units: list[str], measured: list[str], exceptions: list[dict[str, str]]):
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)
        subprocess.run(  # noqa: S603 -- fixed argv
            [GIT, "init", "-q", str(self.root)], check=True, timeout=30, env=CLEAN_ENV
        )
        (self.root / "scripts/ci").mkdir(parents=True)
        baseline = {"measured_sources": measured}
        (self.root / "scripts/ci/tidy-baseline-cpu.json").write_text(json.dumps(baseline))
        (self.root / ".config/lint-exceptions.d").mkdir(parents=True)
        (self.root / LIST).write_text(toml_of(exceptions))
        for unit in units:
            path = self.root / unit
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("int x;\n")
        subprocess.run(  # noqa: S603 -- fixed argv
            [GIT, "-C", str(self.root), "add", "-A"], check=True, timeout=30, env=CLEAN_ENV
        )

    def run(self) -> subprocess.CompletedProcess[str]:
        return subprocess.run(  # noqa: S603 -- fixed argv
            [sys.executable, str(SCRIPT), "--root", str(self.root), "--today", TODAY],
            capture_output=True,
            text=True,
            check=False,
            timeout=60,
            env=CLEAN_ENV,
        )


class Coverage(unittest.TestCase):
    def check(
        self, units: list[str], measured: list[str], exceptions: list[dict[str, str]]
    ) -> subprocess.CompletedProcess[str]:
        fixture = Fixture(units, measured, exceptions)
        self.addCleanup(fixture.dir.cleanup)
        return fixture.run()

    def test_every_unit_read_or_excepted_passes(self) -> None:
        result = self.check(["a.c", "b.mm"], ["a.c"], [entry("b.mm")])
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_a_new_unit_outside_every_lane_fails(self) -> None:
        """The planted defect: a tracked .c that no baseline measures."""
        result = self.check(["a.c", "planted_outside_every_lane.c"], ["a.c"], [])
        self.assertEqual(result.returncode, 1)
        self.assertIn("planted_outside_every_lane.c", result.stdout)

    def test_the_pelorus_mirror_passes_only_through_its_entry(self) -> None:
        mirror = "core/src/interop/pelorus_interop.c"
        planted = "core/src/interop/not_a_mirror.c"
        self.assertEqual(self.check([mirror], [], [entry(mirror)]).returncode, 0)
        self.assertEqual(self.check([mirror], [], []).returncode, 1)
        result = self.check([mirror, planted], [], [entry(mirror)])
        self.assertEqual(result.returncode, 1)
        self.assertIn(planted, result.stdout)
        self.assertNotIn(mirror, result.stdout)

    def test_every_unit_suffix_is_a_unit(self) -> None:
        names = ["u.c", "u.cc", "u.cpp", "u.cxx", "u.cu", "u.hip", "u.mm", "u.metal"]
        result = self.check(names, [], [])
        self.assertEqual(result.returncode, 1)
        for name in names:
            self.assertIn(name, result.stdout)

    def test_headers_and_exact_twin_fragments_are_not_units(self) -> None:
        result = self.check(["a.h", "scripts/ci/exact_twins.d/adm.hip", "c.py"], [], [])
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_an_expired_exception_fails(self) -> None:
        result = self.check(["b.mm"], [], [entry("b.mm", expires="2026-10-04")])
        self.assertEqual(result.returncode, 1)
        self.assertIn("expired", result.stdout)

    def test_an_exception_for_a_unit_a_lane_reads_fails(self) -> None:
        result = self.check(["a.c"], ["a.c"], [entry("a.c")])
        self.assertEqual(result.returncode, 1)
        self.assertIn("delete the entry", result.stdout)

    def test_an_exception_for_a_missing_file_fails(self) -> None:
        result = self.check(["a.c"], ["a.c"], [entry("gone.c")])
        self.assertEqual(result.returncode, 1)
        self.assertIn("not a tracked unit", result.stdout)

    def test_an_exception_needs_a_reason(self) -> None:
        for bad in ({"path": "b.mm", "expires": "2027-01-01"}, entry("b.mm", reason="")):
            with self.subTest(bad=bad):
                result = self.check(["b.mm"], [], [bad])
                self.assertEqual(result.returncode, 1)

    def test_a_bad_date_fails(self) -> None:
        result = self.check(["b.mm"], [], [entry("b.mm", expires="soon")])
        self.assertEqual(result.returncode, 1)

    def test_an_unreadable_exception_list_fails(self) -> None:
        fixture = Fixture(["a.c"], ["a.c"], [])
        self.addCleanup(fixture.dir.cleanup)
        (fixture.root / LIST).write_text("[[exception")
        self.assertEqual(fixture.run().returncode, 1)


class RealTree(unittest.TestCase):
    def test_the_repository_is_covered_today(self) -> None:
        result = subprocess.run(  # noqa: S603 -- fixed argv
            [sys.executable, str(SCRIPT), "--root", str(ROOT)],
            capture_output=True,
            text=True,
            check=False,
            timeout=120,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_every_pelorus_mirror_unit_has_an_entry(self) -> None:
        manifest = (ROOT / "scripts/ci/pelorus-mirror-paths.txt").read_text(encoding="utf-8")
        listed = {
            e["path"] for e in tomllib.loads((ROOT / LIST).read_text(encoding="utf-8"))["exception"]
        }
        for line in manifest.splitlines():
            if line.endswith(".c"):
                self.assertIn(line, listed)

    def test_no_exception_expires_within_the_release_candidates(self) -> None:
        listed = tomllib.loads((ROOT / LIST).read_text(encoding="utf-8"))
        for item in listed["exception"]:
            self.assertGreater(item["expires"], datetime.date(2026, 10, 31), item["path"])


if __name__ == "__main__":
    unittest.main()
