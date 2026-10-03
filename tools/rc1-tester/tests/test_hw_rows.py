# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for the state-row map of the macOS tester bundle (ADR-1496)."""

from __future__ import annotations

import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaf_rc1_tester import hw_rows, hw_suites

ROW = {
    "id": "T-EXAMPLE-2026-10-03",
    "cases": [{"test": "test_metal_x_parity", "case": "test_x_exact"}],
    "metrics": [{"extractor": "x_metal", "metrics": ["x"], "bound": 0}],
    "gate": [{"feature": "x"}],
}
CPU = {"f1": {"x": [1.0, 2.0]}}
GATE_OK = {"fixtures": [{"fixture": "f1", "cells": [{"feature": "x", "status": "OK",
                                                     "max_abs_diff": "0"}]}]}  # fmt: skip


def metal(values, on_metal=("x_metal",)):
    return {"f1": {"scores": {"x": values}, "on_metal": list(on_metal)}}


def evaluate(cases, metal_runs, gate=GATE_OK, row=ROW):
    return hw_rows.evaluate_row(row, cases, CPU, metal_runs, gate)


def test_every_measurement_passing_is_pass() -> None:
    row = evaluate({"test_metal_x_parity": {"test_x_exact": "pass"}}, metal([1.0, 2.0]))
    assert row["verdict"] == "pass"
    assert [item["result"] for item in row["evidence"]] == ["pass", "identical", "OK"]


def test_one_failed_measurement_is_fail() -> None:
    assert (
        evaluate({"test_metal_x_parity": {"test_x_exact": "fail"}}, metal([1.0, 2.0]))["verdict"]
        == "fail"
    )
    row = evaluate({"test_metal_x_parity": {"test_x_exact": "pass"}}, metal([1.0, 2.5]))
    assert row["verdict"] == "fail"
    assert row["evidence"][1]["max_abs_diff"] == "0.5"


def test_skipped_or_missing_measurement_is_not_measured() -> None:
    assert (
        evaluate({"test_metal_x_parity": {"test_x_exact": "skip"}}, metal([1.0, 2.0]))["verdict"]
        == "not_measured"
    )
    assert evaluate({}, metal([1.0, 2.0]))["evidence"][0]["result"] == "not_run"
    assert evaluate({}, {}, gate={"status": "no_device"})["verdict"] == "not_measured"


def test_cpu_fallback_and_missing_metric_fail() -> None:
    cases = {"test_metal_x_parity": {"test_x_exact": "pass"}}
    assert evaluate(cases, metal([1.0, 2.0], on_metal=()))["evidence"][1]["result"] == (
        "not_on_metal"
    )
    absent = {"f1": {"scores": {}, "on_metal": ["x_metal"]}}
    assert evaluate(cases, absent)["evidence"][1]["result"] == "absent"


def test_bound_admits_a_math_library_difference() -> None:
    row = {**ROW, "metrics": [{"extractor": "x_metal", "metrics": ["x"], "bound": 1e-9}]}
    cases = {"test_metal_x_parity": {"test_x_exact": "pass"}}
    result = evaluate(cases, metal([1.0, 2.0 + 5e-10]), row=row)
    assert result["evidence"][1]["result"] == "within_bound" and result["verdict"] == "pass"


def test_section_counts_and_no_map() -> None:
    section = hw_rows.evaluate_rows({"rows": [ROW]}, {}, CPU, {}, {})
    assert section["status"] == "not_measured" and section["counts"]["not_measured"] == 1
    assert hw_rows.evaluate_rows(None, {}, CPU, {}, {})["status"] == "not_applicable"


def test_case_lines_are_parsed_from_test_output() -> None:
    text = (
        "test_a: \x1b[32mpass\x1b[0m\n@case test_a pass\n"
        "test_b: [skip: no Metal device] pass\n@case test_b skip\n"
        "test_c: fail\n@case test_c fail\n@message test_c the twin differs\n"
        "@case not_a_case pass\n"
    )
    verdicts, messages = hw_suites.parse_case_lines(text)
    assert verdicts == {"test_a": "pass", "test_b": "skip", "test_c": "fail"}
    assert messages == {"test_c": "the twin differs"}


def test_feature_left_out_of_a_fixture_is_not_evidence() -> None:
    gate = {"fixtures": [
        {"fixture": "f1", "cells": [{"feature": "x", "status": "OK", "max_abs_diff": "0"}]},
        {"fixture": "f2", "cells": [], "left_out": [{"feature": "x", "reason": "r"}]},
    ]}  # fmt: skip
    items = hw_rows.gate_evidence({"feature": "x"}, gate)
    assert [item["fixture"] for item in items] == ["f1"] and items[0]["result"] == "OK"


def test_cell_the_gate_skips_is_not_evidence() -> None:
    # The gate skips float_ms_ssim_chroma where a chroma plane is below 176 px.
    gate = {"fixtures": [
        {"fixture": "f1", "cells": [{"feature": "x", "status": "SKIP", "max_abs_diff": "0"}]},
        {"fixture": "f2", "cells": [{"feature": "x", "status": "OK", "max_abs_diff": "0"}]},
    ]}  # fmt: skip
    items = hw_rows.gate_evidence({"feature": "x"}, gate)
    assert [item["fixture"] for item in items] == ["f2"]
