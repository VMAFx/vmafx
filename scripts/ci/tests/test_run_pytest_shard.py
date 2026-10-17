#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Positive, negative and boundary controls for scripts/ci/run_pytest_shard.py.

The pure helpers are called directly; the end-to-end cases run the script as a
subprocess of this interpreter on a throwaway test file, as the Coverage Gate
runs it, and read which tests each shard ran from pytest's own output.
"""

from __future__ import annotations

import argparse
import importlib.util
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "scripts" / "ci" / "run_pytest_shard.py"
TIMEOUT_SECONDS = 300
ITEMS = 7

SPEC = importlib.util.spec_from_file_location("run_pytest_shard", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
shard_module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(shard_module)

SAMPLE_TESTS = "\n".join(
    [f"def test_item_{n}():\n    pass\n" for n in range(ITEMS)]
    + ["import pytest\n", "@pytest.mark.slow\ndef test_slow_item():\n    pass\n"]
)
PASSED = re.compile(r"::(test_\w+) PASSED")
DESELECTED = re.compile(r"(\d+) deselected")


class HelperTests(unittest.TestCase):
    def test_parse_shard_accepts_every_index_up_to_count(self) -> None:
        self.assertEqual(shard_module.parse_shard("1/4"), (1, 4))
        self.assertEqual(shard_module.parse_shard("4/4"), (4, 4))
        self.assertEqual(shard_module.parse_shard("1/1"), (1, 1))

    def test_parse_shard_refuses_bad_spellings(self) -> None:
        for text in ("0/4", "5/4", "1/0", "0/0", "4", "a/b", "-1/4", "1/4/2", "", "/4"):
            with self.subTest(text=text), self.assertRaises(argparse.ArgumentTypeError):
                shard_module.parse_shard(text)

    def test_shards_partition_the_selection(self) -> None:
        items = [f"t::{n}" for n in range(ITEMS)]
        for count in (1, 2, 3, ITEMS, ITEMS + 1):
            splits = [shard_module.split_shard(items, i, count) for i in range(1, count + 1)]
            merged = [item for kept, _ in splits for item in kept]
            with self.subTest(count=count):
                self.assertEqual(sorted(merged), sorted(items))
                self.assertEqual(len(merged), len(set(merged)))
                for kept, dropped in splits:
                    self.assertEqual(sorted(kept + dropped), sorted(items))
        self.assertEqual(shard_module.split_shard(items, 2, 3)[0], ["t::1", "t::4"])


class EndToEndTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = Path(self._tmp.name)
        (self.dir / "test_sample.py").write_text(SAMPLE_TESTS, encoding="utf-8")
        (self.dir / "pytest.ini").write_text("[pytest]\nmarkers =\n    slow\n", encoding="utf-8")

    def run_shard(self, shard: str, path: str | None = None) -> subprocess.CompletedProcess[str]:
        command = [sys.executable, str(SCRIPT), "--shard", shard, "--"]
        command += [path or str(self.dir), "-v", "-p", "no:cacheprovider", "-m", "not slow"]
        return subprocess.run(  # noqa: S603 -- fixed argv of this interpreter, no shell
            command,
            capture_output=True,
            text=True,
            check=False,
            cwd=self.dir,
            timeout=TIMEOUT_SECONDS,
        )

    def test_shards_run_every_selected_item_once(self) -> None:
        ran: list[str] = []
        for index in (1, 2, 3):
            result = self.run_shard(f"{index}/3")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            passed = PASSED.findall(result.stdout)
            # Every item outside the shard is reported deselected, the slow item included.
            self.assertEqual(set(DESELECTED.findall(result.stdout)), {str(ITEMS + 1 - len(passed))})
            ran += passed
        self.assertEqual(sorted(ran), sorted(f"test_item_{n}" for n in range(ITEMS)))
        self.assertNotIn("test_slow_item", ran)

    def test_empty_shard_fails_instead_of_running_everything(self) -> None:
        result = self.run_shard(f"{ITEMS + 1}/{ITEMS + 1}")
        self.assertEqual(result.returncode, 5, result.stdout + result.stderr)
        self.assertIn("0 items kept", result.stdout)
        self.assertEqual(PASSED.findall(result.stdout), [])

    def test_failed_collection_fails(self) -> None:
        result = self.run_shard("1/2", str(self.dir / "missing"))
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(PASSED.findall(result.stdout), [])

    def test_bad_shard_is_a_usage_error(self) -> None:
        result = self.run_shard("3/2")
        self.assertEqual(result.returncode, 2)
        self.assertIn("1 <= I <= N", result.stderr)

    def test_ini_below_the_working_directory_keeps_its_rootdir(self) -> None:
        # The Coverage Gate's layout: python/tox.ini sets the rootdir, the run starts at the root.
        project = self.dir / "proj"
        (project / "test").mkdir(parents=True)
        (project / "tox.ini").write_text("[pytest]\nmarkers =\n    slow\n", encoding="utf-8")
        (project / "test" / "test_sample.py").write_text(SAMPLE_TESTS, encoding="utf-8")
        (self.dir / "test_sample.py").unlink()
        (self.dir / "pytest.ini").unlink()
        ran: list[str] = []
        for index in (1, 2):
            result = self.run_shard(f"{index}/2", "proj/test")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn(f"rootdir: {project}", result.stdout)
            ran += PASSED.findall(result.stdout)
        self.assertEqual(sorted(ran), sorted(f"test_item_{n}" for n in range(ITEMS)))

    def test_failing_test_fails_its_shard(self) -> None:
        (self.dir / "test_sample.py").write_text(
            SAMPLE_TESTS + "\ndef test_broken():\n    assert False\n", encoding="utf-8"
        )
        outcomes = [self.run_shard(f"{index}/2").returncode for index in (1, 2)]
        self.assertEqual(sorted(outcomes), [0, 1])


if __name__ == "__main__":
    unittest.main()
