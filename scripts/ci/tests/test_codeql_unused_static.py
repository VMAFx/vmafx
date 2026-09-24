#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests and regressions for CodeQL unused-static CSV parser and validator."""

from __future__ import annotations

import contextlib
import csv
import io
import sys
import tempfile
import unittest
from pathlib import Path

# Add scripts/ci directory to module search path
SCRIPTS_CI_DIR = Path(__file__).resolve().parents[1]
if str(SCRIPTS_CI_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_CI_DIR))

import importlib  # noqa: E402

check_module = importlib.import_module("check-codeql-unused-static")
parse_codeql_csv = check_module.parse_codeql_csv
evaluate_unused_static_results = check_module.evaluate_unused_static_results
CodeQLSchemaError = check_module.CodeQLSchemaError
main = check_module.main
EXPECTED_QUERY_NAME = check_module.EXPECTED_QUERY_NAME
EXPECTED_QUERY_DESCRIPTION = check_module.EXPECTED_QUERY_DESCRIPTION
EXPECTED_SEVERITY = check_module.EXPECTED_SEVERITY

SAMPLE_CLEAN_SIX_ROW_CSV = """\
"Unused static function","A static function that is never called or accessed may be an indication that the code is incomplete or has a typo.","recommendation","Static function picture_compute_geometry is unreachable","/core/src/picture.c","137","13","137","36"
"Unused static function","A static function that is never called or accessed may be an indication that the code is incomplete or has a typo.","recommendation","Static function pool_release_picture is unreachable","/core/src/picture.c","105","12","105","31"
"Unused static function","A static function that is never called or accessed may be an indication that the code is incomplete or has a typo.","recommendation","Static function post_process_feature_from_another is unreachable","/core/src/predict.c","309","12","309","44"
"Unused static function","A static function that is never called or accessed may be an indication that the code is incomplete or has a typo.","recommendation","Static function scan_feature is unreachable ([[""post_process_feature_from_another""|""relative:///core/src/predict.c:309:12:309:44""]] must be removed at the same time)","/core/src/predict.c","282","12","282","23"
"Unused static function","A static function that is never called or accessed may be an indication that the code is incomplete or has a typo.","recommendation","Static function scan_match_feature is unreachable ([[""post_process_feature_from_another""|""relative:///core/src/predict.c:309:12:309:44""]] must be removed at the same time)
Static function scan_match_feature is unreachable ([[""scan_feature""|""relative:///core/src/predict.c:282:12:282:23""]] must be removed at the same time)","/core/src/predict.c","262","12","262","29"
"Unused static function","A static function that is never called or accessed may be an indication that the code is incomplete or has a typo.","recommendation","Static function dump_c_values is unreachable","/core/src/feature/cambi.c","1512","12","1512","24"
"""

SAMPLE_FOUR_COL_RAW_BQRS_CSV = """\
"f","col1","other","col3"
"picture_compute_geometry","Static function picture_compute_geometry is unreachable","picture_compute_geometry","picture_compute_geometry"
"pool_release_picture","Static function pool_release_picture is unreachable","pool_release_picture","pool_release_picture"
"""


def official_row(
    *,
    path: str = "/core/src/predict.c",
    name: str = EXPECTED_QUERY_NAME,
    description: str = EXPECTED_QUERY_DESCRIPTION,
    severity: str = EXPECTED_SEVERITY,
    message: str = "Static function dead_helper is unreachable",
    start_line: str = "10",
    start_column: str = "1",
    end_line: str = "10",
    end_column: str = "15",
) -> str:
    stream = io.StringIO(newline="")
    csv.writer(stream).writerow(
        [
            name,
            description,
            severity,
            message,
            path,
            start_line,
            start_column,
            end_line,
            end_column,
        ]
    )
    return stream.getvalue()


def without_picture_rows(csv_text: str) -> str:
    output = io.StringIO(newline="")
    writer = csv.writer(output)
    writer.writerows(
        row for row in csv.reader(io.StringIO(csv_text)) if row[4] != "/core/src/picture.c"
    )
    return output.getvalue()


SAMPLE_CORRECTED_FOUR_ROW_CSV = without_picture_rows(SAMPLE_CLEAN_SIX_ROW_CSV)
SAMPLE_TARGET_VIOLATION_CSV = official_row(path="/core/src/thread_pool.c")


def run_main(args: list[str]) -> tuple[int, str, str]:
    stdout = io.StringIO()
    stderr = io.StringIO()
    with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
        result = main(args)
    return result, stdout.getvalue(), stderr.getvalue()


class CodeQLUnusedStaticParserTests(unittest.TestCase):
    def test_baseline_six_rows_expose_picture_target_violations(self) -> None:
        rows = parse_codeql_csv(io.StringIO(SAMPLE_CLEAN_SIX_ROW_CSV))
        self.assertEqual(len(rows), 6)
        violations, untargeted = evaluate_unused_static_results(rows)
        self.assertEqual(
            [row["path"] for row in violations],
            ["core/src/picture.c", "core/src/picture.c"],
        )
        self.assertEqual(len(untargeted), 4)

    def test_positive_corrected_four_row_inventory_passes(self) -> None:
        rows = parse_codeql_csv(io.StringIO(SAMPLE_CORRECTED_FOUR_ROW_CSV))
        self.assertEqual(len(rows), 4)
        violations, untargeted = evaluate_unused_static_results(rows)
        self.assertEqual(violations, [])
        self.assertEqual(len(untargeted), 4)
        self.assertEqual(rows[0]["path"], "core/src/predict.c")
        self.assertEqual(rows[3]["path"], "core/src/feature/cambi.c")

    def test_negative_target_path_violations_fail(self) -> None:
        for target in check_module.DEFAULT_LANE_TARGET_PATHS:
            csv_content = official_row(path=f"/{target}")
            rows = parse_codeql_csv(io.StringIO(csv_content))
            violations, _ = evaluate_unused_static_results(rows)
            self.assertEqual(len(violations), 1)
            self.assertEqual(violations[0]["path"], target)

    def test_schema_raw_four_column_bqrs_fails_closed(self) -> None:
        """Raw 4-column BQRS CSV must be rejected by the 9-column schema validator."""
        with self.assertRaises(CodeQLSchemaError) as ctx:
            parse_codeql_csv(io.StringIO(SAMPLE_FOUR_COL_RAW_BQRS_CSV))
        self.assertIn("expected 9 columns, got 4", str(ctx.exception))

    def test_schema_truncated_fewer_than_nine_columns_fails_closed(self) -> None:
        for col_count in (1, 2, 3, 4, 5, 6, 7, 8):
            row = ",".join(f'"val{i}"' for i in range(col_count)) + "\n"
            with self.assertRaises(CodeQLSchemaError) as ctx:
                parse_codeql_csv(io.StringIO(row))
            self.assertIn(f"expected 9 columns, got {col_count}", str(ctx.exception))

    def test_schema_extra_columns_fails_closed(self) -> None:
        row = ",".join(f'"val{i}"' for i in range(10)) + "\n"
        with self.assertRaises(CodeQLSchemaError) as ctx:
            parse_codeql_csv(io.StringIO(row))
        self.assertIn("expected 9 columns, got 10", str(ctx.exception))

    def test_schema_non_integer_coordinates_fails_closed(self) -> None:
        row = official_row(start_line="invalid")
        with self.assertRaises(CodeQLSchemaError) as ctx:
            parse_codeql_csv(io.StringIO(row))
        self.assertIn("non-integer location coordinates", str(ctx.exception))

    def test_schema_zero_or_negative_coordinates_fails_closed(self) -> None:
        row = official_row(start_line="0")
        with self.assertRaises(CodeQLSchemaError) as ctx:
            parse_codeql_csv(io.StringIO(row))
        self.assertIn("invalid location coordinates", str(ctx.exception))

    def test_schema_inverted_line_range_fails_closed(self) -> None:
        row = official_row(start_line="20", end_line="10")
        with self.assertRaises(CodeQLSchemaError) as ctx:
            parse_codeql_csv(io.StringIO(row))
        self.assertIn("invalid location coordinates", str(ctx.exception))

    def test_schema_empty_path_fails_closed(self) -> None:
        row = official_row(path="")
        with self.assertRaises(CodeQLSchemaError) as ctx:
            parse_codeql_csv(io.StringIO(row))
        self.assertIn("empty file path", str(ctx.exception))

    def test_schema_query_identity_fields_fail_closed(self) -> None:
        invalid_fields = (
            (official_row(name="Different query"), "unexpected query name"),
            (official_row(description="Different description"), "unexpected query description"),
            (official_row(severity="warning"), "unexpected severity"),
            (official_row(message="Different diagnostic"), "unexpected unused-static message"),
        )
        for row, expected in invalid_fields:
            with self.subTest(expected=expected):
                with self.assertRaises(CodeQLSchemaError) as ctx:
                    parse_codeql_csv(io.StringIO(row))
                self.assertIn(expected, str(ctx.exception))

    def test_main_cli_returns_expected_exit_codes(self) -> None:
        with tempfile.NamedTemporaryFile("w+", encoding="utf-8", delete=False) as tf:
            tf.write(SAMPLE_CORRECTED_FOUR_ROW_CSV)
            clean_path = tf.name
        try:
            result, stdout, stderr = run_main([clean_path])
            self.assertEqual(result, 0)
            self.assertIn("(4 repository-wide row(s) inventoried)", stdout)
            self.assertEqual(stderr, "")
        finally:
            Path(clean_path).unlink(missing_ok=True)

        with tempfile.NamedTemporaryFile("w+", encoding="utf-8", delete=False) as tf:
            tf.write(SAMPLE_TARGET_VIOLATION_CSV)
            violation_path = tf.name
        try:
            result, stdout, stderr = run_main([violation_path])
            self.assertEqual(result, 1)
            self.assertEqual(stdout, "")
            self.assertIn("1 selected unused-static finding(s) remain", stderr)
        finally:
            Path(violation_path).unlink(missing_ok=True)

        with tempfile.NamedTemporaryFile("w+", encoding="utf-8", delete=False) as tf:
            tf.write(SAMPLE_FOUR_COL_RAW_BQRS_CSV)
            schema_err_path = tf.name
        try:
            result, stdout, stderr = run_main([schema_err_path])
            self.assertEqual(result, 2)
            self.assertEqual(stdout, "")
            self.assertIn("FAIL (schema error)", stderr)
        finally:
            Path(schema_err_path).unlink(missing_ok=True)


if __name__ == "__main__":
    unittest.main()
