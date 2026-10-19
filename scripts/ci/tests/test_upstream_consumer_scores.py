# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
"""Tests for scripts/ci/upstream_consumer_scores.py (positive, negative, boundary)."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import upstream_consumer_scores as ucs


def _doc(vmaf: tuple[str, ...], extra: str = "1.000000") -> str:
    """JSON text with literal numbers, one frame per element of ``vmaf``."""
    frames = ",".join(
        f'{{"frameNum": {i}, "metrics": {{"vmaf": {v}, "integer_adm2": {extra}}}}}'
        for i, v in enumerate(vmaf)
    )
    return f'{{"frames": [{frames}], "pooled_metrics": {{"vmaf": {{"mean": {vmaf[0]}}}}}}}'


class ScoreTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = Path(self._tmp.name)

    def _write(self, name: str, text: str) -> Path:
        p = self.dir / name
        p.write_text(text, encoding="utf-8")
        return p

    def test_identical_files_pass(self) -> None:
        a = self._write("a.json", _doc(("83.856284", "82.1", "80.0")))
        b = self._write("b.json", _doc(("83.856284", "82.1", "80.0")))
        self.assertEqual(ucs.main([str(a), str(b), "--pooled"]), ucs.EXIT_SAME)

    def test_one_ulp_difference_fails(self) -> None:
        # 83.856284 vs the next double up: text differs in the last digit only.
        a = self._write("a.json", _doc(("83.85628400000001",)))
        b = self._write("b.json", _doc(("83.85628400000002",)))
        diffs = ucs.compare(ucs.load_scores(a), ucs.load_scores(b), "ff", "cli")
        self.assertEqual(len(diffs), 1)
        self.assertIn("frame 0 vmaf", diffs[0])
        self.assertEqual(ucs.main([str(a), str(b)]), ucs.EXIT_DIFF)

    def test_trailing_zero_is_a_difference(self) -> None:
        a = self._write("a.json", _doc(("83.5",)))
        b = self._write("b.json", _doc(("83.50",)))
        self.assertEqual(ucs.main([str(a), str(b)]), ucs.EXIT_DIFF)

    def test_missing_frame_fails(self) -> None:
        a = self._write("a.json", _doc(("1.0", "2.0", "3.0")))
        b = self._write("b.json", _doc(("1.0", "2.0")))
        diffs = ucs.compare(ucs.load_scores(a), ucs.load_scores(b), "a", "b")
        self.assertEqual(diffs, ["frame 2: missing from b"])
        self.assertEqual(ucs.main([str(a), str(b)]), ucs.EXIT_DIFF)

    def test_missing_metric_fails(self) -> None:
        a = self._write("a.json", _doc(("1.0",)))
        b = self._write(
            "b.json",
            '{"frames": [{"frameNum": 0, "metrics": {"vmaf": 1.0}}]}',
        )
        diffs = ucs.compare(ucs.load_scores(a), ucs.load_scores(b))
        self.assertEqual(len(diffs), 1)
        self.assertIn("integer_adm2", diffs[0])

    def test_pooled_difference_only_with_flag(self) -> None:
        a = self._write("a.json", _doc(("1.0",)))
        b = self._write("b.json", a.read_text().replace('"mean": 1.0', '"mean": 1.5'))
        self.assertEqual(ucs.main([str(a), str(b)]), ucs.EXIT_SAME)
        self.assertEqual(ucs.main([str(a), str(b), "--pooled"]), ucs.EXIT_DIFF)

    def test_rename_keeps_values_exact(self) -> None:
        a = self._write("a.json", _doc(("83.856284",)).replace('"vmaf"', '"self"'))
        b = self._write("b.json", _doc(("83.856284",)))
        self.assertEqual(ucs.main([str(a), str(b)]), ucs.EXIT_DIFF)
        self.assertEqual(
            ucs.main([str(a), str(b), "--pooled", "--rename-a", "self=vmaf"]),
            ucs.EXIT_SAME,
        )
        c = self._write("c.json", _doc(("83.856285",)).replace('"vmaf"', '"self"'))
        self.assertEqual(ucs.main([str(c), str(b), "--rename-a", "self=vmaf"]), ucs.EXIT_DIFF)

    def test_rename_onto_existing_metric_is_a_setup_error(self) -> None:
        a = self._write("a.json", _doc(("1.0",)))
        b = self._write("b.json", _doc(("1.0",)))
        self.assertEqual(
            ucs.main([str(a), str(b), "--rename-a", "integer_adm2=vmaf"]),
            ucs.EXIT_SETUP,
        )
        self.assertEqual(ucs.main([str(a), str(b), "--rename-a", "bad"]), ucs.EXIT_SETUP)

    def test_unreadable_inputs_are_setup_errors(self) -> None:
        good = self._write("g.json", _doc(("1.0",)))
        bad = self._write("bad.json", "not json")
        empty = self._write("empty.json", '{"frames": []}')
        self.assertEqual(ucs.main([str(good), str(self.dir / "nope.json")]), ucs.EXIT_SETUP)
        self.assertEqual(ucs.main([str(good), str(bad)]), ucs.EXIT_SETUP)
        self.assertEqual(ucs.main([str(good), str(empty)]), ucs.EXIT_SETUP)

    def test_duplicate_frame_is_a_setup_error(self) -> None:
        text = (
            '{"frames": [{"frameNum": 0, "metrics": {"vmaf": 1.0}},'
            ' {"frameNum": 0, "metrics": {"vmaf": 1.0}}]}'
        )
        p = self._write("dup.json", text)
        with self.assertRaises(ucs.ScoreFileError):
            ucs.load_scores(p)


if __name__ == "__main__":
    unittest.main()
