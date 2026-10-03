# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for scripts/ci/check-hardware-reports.py and the index generator."""

from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

import pytest

jsonschema = pytest.importorskip("jsonschema")

_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(_ROOT / "tools" / "rc1-tester" / "src"))

from vmaf_rc1_tester.hw_report import report_digest


def load(path: str):
    spec = importlib.util.spec_from_file_location(Path(path).stem.replace("-", "_"), _ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


gate = load("scripts/ci/check-hardware-reports.py")
index = load("scripts/docs/generate-hardware-reports.py")

CELL = {"status": "identical", "fixtures": []}
SUITE = {"status": "pass", "total": 2, "passed": 2, "failed": 0, "skipped": 0, "failures": []}


def good_report() -> dict:
    report = {
        "schema_version": "1",
        "tool": {"name": "vmaf-tester-report", "version": "0.1.0"},
        "generated_utc": "2026-10-03T10:00:00Z",
        "image": {
            "kind": "container-image", "source_commit": "a" * 40, "recipe_commit": "a" * 40, "source_ref": "refs/tags/v1.0.0-rc.2",
            "built_by_workflow": True, "image_arch": "aarch64", "compiler": "gcc",
            "libc": "glibc", "base_image": "debian", "tag": "v1.0.0-rc.2-tester",
            "digest": None, "vmaf_version": "1.0.0-rc.2", "vmaf_sha256": "b" * 64,
            "libvmaf_sha256": "c" * 64, "files_match_build": True, "python": "3.14.8",
        },
        "host": {
            "platform": "linux", "machine": "aarch64", "kernel_release": "6.1", "in_container": True,
            "cpu_model": "implementer 0x61, part 0x32", "cpuinfo": {"cpu part": "0x32"},
            "cpu_features": ["asimd"], "hwcap": 1, "hwcap2": 0, "sve_in_hwcap": False,
            "dispatch_flags": ["neon"], "dispatch_flags_source": "x", "logical_cores": 4,
        },
        "dispatch_equivalence": CELL,
        "metal_equivalence": {"status": "not_applicable"},
        "reference_equivalence": {"status": "identical"},
        "unit_tests": SUITE,
        "golden_gate": SUITE,
        "not_exercised": [{"item": "Metal", "reason": "Linux"}],
        "verdict": "pass",
        "failed_checks": [],
        "note": "",
    }  # fmt: skip
    report["report_sha256"] = report_digest(report)
    return report


def write(directory: Path, name: str, report: dict) -> None:
    schema = _ROOT / "docs" / "hardware-reports" / "report.schema.json"
    (directory / "report.schema.json").write_text(schema.read_text())
    (directory / name).write_text(json.dumps(report))


GOOD_NAME = "2026-10-03-apple-m4.json"


def run(directory: Path) -> int:
    return gate.check(directory)


def test_good_report_is_accepted(tmp_path: Path) -> None:
    write(tmp_path, GOOD_NAME, good_report())
    assert run(tmp_path) == 0


def test_no_reports_is_clean(tmp_path: Path) -> None:
    assert run(tmp_path) == 0


def test_hand_edit_breaks_the_hash(tmp_path: Path) -> None:
    report = good_report()
    report["host"]["cpu_model"] = "edited"
    write(tmp_path, GOOD_NAME, report)
    assert run(tmp_path) == 1


def test_note_may_be_edited_after_the_run(tmp_path: Path) -> None:
    report = good_report()
    report["note"] = "Apple M4, Docker Desktop"
    write(tmp_path, GOOD_NAME, report)
    assert run(tmp_path) == 0


@pytest.mark.parametrize(
    "name", ["report.json", "2026-10-03.json", "2026-10-04-apple-m4.json", "2026-10-03-A.json"]
)
def test_bad_file_names_are_refused(tmp_path: Path, name: str) -> None:
    write(tmp_path, name, good_report())
    assert run(tmp_path) == 1


def test_schema_violation_and_foreign_image_are_refused(tmp_path: Path) -> None:
    report = good_report()
    del report["host"]
    write(tmp_path, GOOD_NAME, report)
    assert run(tmp_path) == 1
    foreign = good_report()
    foreign["image"]["built_by_workflow"] = False
    foreign["report_sha256"] = report_digest(foreign)
    write(tmp_path, GOOD_NAME, foreign)
    assert run(tmp_path) == 1


def test_cpuinfo_key_outside_allow_list_is_refused(tmp_path: Path) -> None:
    report = good_report()
    report["host"]["cpuinfo"]["serial"] = "123"
    report["report_sha256"] = report_digest(report)
    write(tmp_path, GOOD_NAME, report)
    assert run(tmp_path) == 1


def test_verdicts(tmp_path: Path) -> None:
    lying = good_report()
    lying["unit_tests"] = {**SUITE, "status": "fail", "failed": 1}
    lying["report_sha256"] = report_digest(lying)
    write(tmp_path, GOOD_NAME, lying)
    assert run(tmp_path) == 1  # verdict pass with a failing section
    honest = good_report()
    honest["unit_tests"] = {**SUITE, "status": "fail", "failed": 1, "failures": ["test_x"]}
    honest["verdict"], honest["failed_checks"] = "fail", ["unit_tests"]
    honest["report_sha256"] = report_digest(honest)
    write(tmp_path, GOOD_NAME, honest)
    assert run(tmp_path) == 0  # a failing report is accepted: it is a finding
    incomplete = good_report()
    incomplete["verdict"] = "incomplete"
    incomplete["report_sha256"] = report_digest(incomplete)
    write(tmp_path, GOOD_NAME, incomplete)
    assert run(tmp_path) == 1


def test_index_lists_reports_and_is_empty_without(tmp_path: Path) -> None:
    assert "No reports" in index.render(tmp_path)
    write(tmp_path, GOOD_NAME, good_report())
    text = index.render(tmp_path)
    assert "| 2026-10-03 | implementer 0x61, part 0x32 | aarch64 | neon | pass |" in text
    assert f"[{GOOD_NAME}]({GOOD_NAME})" in text


def v2_report(**sections) -> dict:
    """A schema 2 report (ADR-1496): the Metal gate and the state-row map."""
    report = good_report()
    report["schema_version"] = "2"
    report["metal_gate"] = {"status": "not_applicable"}
    report["metal_rows"] = {"status": "not_applicable", "rows": []}
    report["unit_tests"] = {**SUITE, "cases": {"test_metal_x_parity": {"test_x": "skip"}}}
    report.update(sections)
    report["report_sha256"] = report_digest(report)
    return report


def test_schema_2_report_with_gate_and_rows_is_accepted(tmp_path: Path) -> None:
    rows = {
        "status": "fail",
        "counts": {"pass": 0, "fail": 1, "not_measured": 0},
        "rows": [{"id": "T-X-2026-10-03", "verdict": "fail", "evidence": [
            {"kind": "case", "test": "test_metal_x_parity", "case": "test_x", "result": "fail"},
            {"kind": "metric", "fixture": "f", "extractor": "x_metal", "metric": "x",
             "result": "differing", "max_abs_diff": "1e-07"},
        ]}],
    }  # fmt: skip
    gate_cells = {"status": "fail", "fixtures": [{"fixture": "f", "left_out": [], "exit_code": 1,
        "cells": [{"feature": "x", "status": "FAIL", "tolerance": "0", "frames": 2,
                   "tolerance_source": "held-exact:ADR-1496", "max_abs_diff": "1e-07",
                   "mismatches": 1, "note": ""}]}]}  # fmt: skip
    report = v2_report(metal_rows=rows, metal_gate=gate_cells, verdict="fail",
                       failed_checks=["metal_gate"])  # fmt: skip
    write(tmp_path, GOOD_NAME, report)
    assert run(tmp_path) == 0


def test_schema_2_report_needs_its_sections(tmp_path: Path) -> None:
    report = v2_report()
    del report["metal_rows"]
    report["report_sha256"] = report_digest(report)
    write(tmp_path, GOOD_NAME, report)
    assert run(tmp_path) == 1


def test_failing_metal_gate_cannot_pass(tmp_path: Path) -> None:
    report = v2_report(metal_gate={"status": "fail", "fixtures": []})
    write(tmp_path, GOOD_NAME, report)
    assert run(tmp_path) == 1
