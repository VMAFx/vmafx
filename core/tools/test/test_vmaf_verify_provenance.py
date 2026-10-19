#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""`vmaf --verify-provenance <report>` (#2142, ADR-2073, RC4 WP5).

Usage: test_vmaf_verify_provenance.py <vmaf binary> <repository root>

A fresh JSON report verifies (exit 0) and leaves no re-run report behind. A
planted change fails (exit 1) and the message names the field: the model
digest, the backend, a score, the record digest. A report the check cannot
use fails with exit 2: XML, and a report without a recorded command line.

Failing first: on the WP8 base the option does not exist (exit 1 from the
option parser, without a field name).
"""

from __future__ import annotations

import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from provenance_fixture import score

ARGC = 3  # program, vmaf binary, repository root
BINARY: list[str] = []


def verify(report: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(  # noqa: S603 -- the binary under test, argv built here
        [BINARY[0], "--verify-provenance", str(report)],
        capture_output=True,
        text=True,
        check=False,
        timeout=300,
    )


class VerifyProvenanceTest(unittest.TestCase):
    _tmp: tempfile.TemporaryDirectory[str]
    directory: Path
    report: Path
    text: str

    @classmethod
    def setUpClass(cls) -> None:
        cls._tmp = tempfile.TemporaryDirectory()
        cls.directory = Path(cls._tmp.name)
        cls.report = cls.directory / "report.json"
        result = score(BINARY[0], cls.directory, cls.report, "--json", "--precision", "max")
        if result.returncode != 0:
            raise RuntimeError(result.stderr)
        cls.text = cls.report.read_text()

    @classmethod
    def tearDownClass(cls) -> None:
        cls._tmp.cleanup()

    def planted(self, name: str, pattern: str, replacement: str) -> Path:
        text, count = re.subn(pattern, replacement, self.text, count=1)
        self.assertEqual(count, 1, pattern)
        path = self.directory / name
        path.write_text(text)
        return path

    def assert_named(self, report: Path, field: str) -> None:
        result = verify(report)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn(f"provenance mismatch: {field}:", result.stderr)

    def test_fresh_report_verifies(self) -> None:
        result = verify(self.report)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("provenance verified", result.stdout)
        self.assertFalse(Path(str(self.report) + ".rerun.json").exists())

    def test_planted_model_digest(self) -> None:
        report = self.planted("model.json", r'"sha256":"[0-9a-f]', '"sha256":"x')
        self.assert_named(report, "provenance.models[0].sha256")
        self.assertTrue(Path(str(report) + ".rerun.json").exists())

    def test_planted_backend(self) -> None:
        report = self.planted("backend.json", '"active_backend":"cpu"', '"active_backend":"cuda"')
        self.assert_named(report, "provenance.active_backend")

    def test_planted_score(self) -> None:
        report = self.planted("score.json", r'("vmaf": )(\d)', r"\g<1>1\g<2>")
        self.assert_named(report, "frames[0].metrics.vmaf")

    def test_planted_digest(self) -> None:
        report = self.planted("digest.json", '"digest":"sha256:', '"digest":"sha256:0')
        self.assert_named(report, "provenance.digest")

    def test_xml_report_is_refused(self) -> None:
        xml = self.directory / "report.xml"
        result = score(BINARY[0], self.directory, xml, "--xml")
        self.assertEqual(result.returncode, 0, result.stderr)
        refused = verify(xml)
        self.assertEqual(refused.returncode, 2, refused.stderr)
        self.assertIn("not a JSON report", refused.stderr)

    def test_report_without_command_line(self) -> None:
        record = json.loads(self.text)
        record["provenance"]["annotations"] = []
        path = self.directory / "no_argv.json"
        path.write_text(json.dumps(record))
        result = verify(path)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("records no command line", result.stderr)


def main() -> int:
    if len(sys.argv) != ARGC:
        print(__doc__)
        return 2
    BINARY.append(sys.argv[1])
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(VerifyProvenanceTest)
    return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
