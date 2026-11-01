# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for check_required_tests_ran.py."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from scripts.ci.check_required_tests_ran import (
    CheckRequiredTestsError,
    main,
    verify_junit_tests,
)

SAMPLE_PASSING_XML = """<?xml version="1.0" encoding="utf-8"?>
<testsuites name="pytest tests">
  <testsuite name="pytest" errors="0" failures="0" skipped="0" tests="2">
    <testcase classname="test.hdr_models_pin_test.HdrModelsPinTest" name="test_hdr_v118_1d5h_2160_tagged" time="0.100" />
    <testcase classname="test.vmaf_v1_quality_runner_test.VmafV1QualityRunnerTest" name="test_hdr_v118_3d0h_converts_bt2020nc_to_ictcp_exec" time="0.200" />
  </testsuite>
</testsuites>
"""

SAMPLE_SKIPPED_XML = """<?xml version="1.0" encoding="utf-8"?>
<testsuites name="pytest tests">
  <testsuite name="pytest" errors="0" failures="0" skipped="1" tests="2">
    <testcase classname="test.hdr_models_pin_test.HdrModelsPinTest" name="test_hdr_v118_1d5h_2160_tagged" time="0.100" />
    <testcase classname="test.vmaf_v1_quality_runner_test.VmafV1QualityRunnerTest" name="test_hdr_v118_3d0h_converts_bt2020nc_to_ictcp_exec" time="0.001">
      <skipped message="vmaf was built without zimg support" type="pytest.skip">vmaf was built without zimg support</skipped>
    </testcase>
  </testsuite>
</testsuites>
"""

SAMPLE_ZERO_TESTS_XML = """<?xml version="1.0" encoding="utf-8"?>
<testsuites name="pytest tests">
  <testsuite name="pytest" errors="0" failures="0" skipped="0" tests="0">
  </testsuite>
</testsuites>
"""


class TestCheckRequiredTestsRan(unittest.TestCase):
    """Verify positive, skipped row refused, absent row refused, and zero tests refused."""

    def test_positive(self) -> None:
        """Required tests are present and passed."""
        with tempfile.TemporaryDirectory() as tmp:
            xml_file = Path(tmp) / "junit.xml"
            xml_file.write_text(SAMPLE_PASSING_XML, encoding="utf-8")
            verified = verify_junit_tests(
                xml_file,
                ["hdr_models_pin_test", "vmaf_v1_quality_runner_test"],
            )
            self.assertEqual(len(verified), 2)
            exit_code = main(["--junit", str(xml_file), "--required", "hdr_models_pin_test"])
            self.assertEqual(exit_code, 0)

    def test_skipped_row_refused(self) -> None:
        """A required test that was skipped is refused."""
        with tempfile.TemporaryDirectory() as tmp:
            xml_file = Path(tmp) / "junit.xml"
            xml_file.write_text(SAMPLE_SKIPPED_XML, encoding="utf-8")
            with self.assertRaises(CheckRequiredTestsError) as ctx:
                verify_junit_tests(xml_file, ["vmaf_v1_quality_runner_test"])
            self.assertIn("skipped row refused", str(ctx.exception))
            exit_code = main(
                ["--junit", str(xml_file), "--required", "vmaf_v1_quality_runner_test"]
            )
            self.assertEqual(exit_code, 1)

    def test_absent_row_refused(self) -> None:
        """A required test not found in the XML is refused."""
        with tempfile.TemporaryDirectory() as tmp:
            xml_file = Path(tmp) / "junit.xml"
            xml_file.write_text(SAMPLE_PASSING_XML, encoding="utf-8")
            with self.assertRaises(CheckRequiredTestsError) as ctx:
                verify_junit_tests(xml_file, ["non_existent_test"])
            self.assertIn("absent row refused", str(ctx.exception))
            exit_code = main(["--junit", str(xml_file), "--required", "non_existent_test"])
            self.assertEqual(exit_code, 1)

    def test_zero_tests_refused(self) -> None:
        """A JUnit XML with zero test cases is refused."""
        with tempfile.TemporaryDirectory() as tmp:
            xml_file = Path(tmp) / "junit.xml"
            xml_file.write_text(SAMPLE_ZERO_TESTS_XML, encoding="utf-8")
            with self.assertRaises(CheckRequiredTestsError) as ctx:
                verify_junit_tests(xml_file, ["hdr_models_pin_test"])
            self.assertIn("zero tests refused", str(ctx.exception))
            exit_code = main(["--junit", str(xml_file), "--required", "hdr_models_pin_test"])
            self.assertEqual(exit_code, 1)


if __name__ == "__main__":
    unittest.main()
