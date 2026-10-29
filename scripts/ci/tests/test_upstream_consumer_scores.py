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


def _xml(vmaf: tuple[str, ...], extra: str = "1.000000") -> str:
    """libvmaf XML text with the frames and pooled mean of ``_doc``."""
    frames = "".join(
        f'<frame frameNum="{i}" vmaf="{v}" integer_adm2="{extra}" />' for i, v in enumerate(vmaf)
    )
    return (
        '<VMAF version="t"><frames>'
        + frames
        + '</frames><pooled_metrics><metric name="vmaf" mean="'
        + vmaf[0]
        + '" /></pooled_metrics></VMAF>'
    )


def _csv(vmaf: tuple[str, ...], extra: str = "1.000000") -> str:
    """libvmaf CSV text (trailing comma on every line) with the frames of ``_doc``."""
    rows = "".join(f"{i},{v},{extra},\n" for i, v in enumerate(vmaf))
    return "Frame,vmaf,integer_adm2,\n" + rows


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


class FormatTests(unittest.TestCase):
    """XML and CSV reports compare exactly like JSON reports (#2237)."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = Path(self._tmp.name)
        self.vmaf = ("83.856284", "82.639805", "80.000000")
        self.json = self._write("r.json", _doc(self.vmaf))

    def _write(self, name: str, text: str) -> Path:
        p = self.dir / name
        p.write_text(text, encoding="utf-8")
        return p

    def test_xml_matches_json(self) -> None:
        x = self._write("r.xml", _xml(self.vmaf))
        self.assertEqual(ucs.main([str(self.json), str(x), "--pooled"]), ucs.EXIT_SAME)

    def test_csv_matches_json(self) -> None:
        c = self._write("r.csv", _csv(self.vmaf))
        self.assertEqual(ucs.main([str(self.json), str(c)]), ucs.EXIT_SAME)

    def test_csv_without_trailing_comma(self) -> None:
        c = self._write("r.csv", _csv(self.vmaf).replace(",\n", "\n"))
        self.assertEqual(ucs.main([str(self.json), str(c)]), ucs.EXIT_SAME)

    def test_one_digit_in_xml_fails(self) -> None:
        x = self._write("r.xml", _xml(("83.856284", "82.639806", "80.000000")))
        self.assertEqual(ucs.main([str(self.json), str(x)]), ucs.EXIT_DIFF)

    def test_one_digit_in_csv_fails(self) -> None:
        c = self._write("r.csv", _csv(self.vmaf, extra="1.000001"))
        self.assertEqual(ucs.main([str(self.json), str(c)]), ucs.EXIT_DIFF)

    def test_xml_pooled_difference_with_flag(self) -> None:
        x = self._write("r.xml", _xml(self.vmaf).replace('mean="83.856284"', 'mean="83.5"'))
        self.assertEqual(ucs.main([str(self.json), str(x)]), ucs.EXIT_SAME)
        self.assertEqual(ucs.main([str(self.json), str(x), "--pooled"]), ucs.EXIT_DIFF)

    def test_format_override_reads_unknown_suffix(self) -> None:
        x = self._write("r.txt", _xml(self.vmaf))
        y = self._write("s.txt", _xml(self.vmaf))
        self.assertEqual(ucs.main([str(x), str(y)]), ucs.EXIT_SETUP)
        self.assertEqual(ucs.main([str(x), str(y), "--format", "xml"]), ucs.EXIT_SAME)

    def test_malformed_files_are_setup_errors(self) -> None:
        cases = {
            "broken.xml": "<VMAF><frames>",
            "noframes.xml": "<VMAF><pooled_metrics /></VMAF>",
            "empty.xml": "<VMAF><frames></frames></VMAF>",
            "dup.xml": '<VMAF><frames><frame frameNum="0" vmaf="1" />'
            '<frame frameNum="0" vmaf="1" /></frames></VMAF>',
            "noid.xml": '<VMAF><frames><frame vmaf="1" /></frames></VMAF>',
            "header.csv": "frame,vmaf,\n0,1,\n",
            "cells.csv": "Frame,vmaf,integer_adm2,\n0,1,\n",
            "dup.csv": "Frame,vmaf,\n0,1,\n0,1,\n",
            "badnum.csv": "Frame,vmaf,\nx,1,\n",
            "empty.csv": "Frame,vmaf,\n",
        }
        for name, text in cases.items():
            with self.subTest(name=name):
                p = self._write(name, text)
                self.assertEqual(ucs.main([str(self.json), str(p)]), ucs.EXIT_SETUP)

    def test_csv_has_no_pooled_values(self) -> None:
        c = self._write("r.csv", _csv(self.vmaf))
        self.assertEqual(ucs.load(c)[1], {})


if __name__ == "__main__":
    unittest.main()
