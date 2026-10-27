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
    assert "| 2026-10-03 | implementer 0x61, part 0x32 | aarch64 | neon | - | pass |" in text
    assert f"[{GOOD_NAME}]({GOOD_NAME})" in text
    write(tmp_path, GOOD_NAME, good_gpu_report())
    assert "| neon | Intel(R) UHD Graphics 770 (xe-lp) | pass |" in index.render(tmp_path)


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


# ---- schema 3: the GPU section --------------------------------------------------------------


def gpu_device(status: str = "pass") -> dict:
    return {
        "index": 0, "selector": {"ONEAPI_DEVICE_SELECTOR": "level_zero:0"},
        "facts": {"name": "Intel(R) UHD Graphics 770", "family": "xe-lp", "ip_version": "12.2.0",
                  "sub_group_sizes": [8, 16, 32], "integrated": True},
        "device_lines": ["libvmaf INFO SYCL: using device: Intel(R) UHD Graphics 770"],
        "status": status,
        "twins": {"status": "identical", "fixtures": [], "features": [
            {"feature": "psnr", "bound": "0", "source": "exact:ADR-1397", "status": "identical",
             "values": 3, "differing_values": 0, "max_abs_diff": "0"}]},
        "gate": {"status": "pass", "fixtures": []},
        "device_tests": {**SUITE, "results": {"test_a": "pass", "test_b": "pass"}},
        "audits": {"test_sycl_kernel_scratch": {"status": "pass", "row_result": "pass",
                                                "kernels_audited": 127, "kernels_in_scratch": 0}},
        "rows": {"status": "pass", "counts": {"pass": 1, "fail": 0, "not_measured": 0,
                                               "not_applicable": 2},
                 "rows": [{"id": "T-SYCL-ROW-KERNELS-SG16-OTHER-DEVICES-2026-10-02", "part": "Xe-LP",
                           "verdict": "pass", "evidence": [
                               {"kind": "test", "test": "test_a", "result": "pass"},
                               {"kind": "audit", "test": "test_sycl_kernel_scratch", "result": "pass"}]}]},
    }  # fmt: skip


def good_gpu_report() -> dict:
    report = good_report()
    report["schema_version"] = "3"
    report["image"]["gpu_backend"] = "sycl"
    report["image"]["image_arch"] = "x86_64"
    report["metal_gate"] = {"status": "not_applicable"}
    report["metal_rows"] = {"status": "not_applicable", "rows": []}
    report["gpu"] = {"status": "pass", "backend": "sycl", "access": {"path": "drm"},
                     "runtime": {"libze1": "1.34.0"}, "devices": [gpu_device()]}  # fmt: skip
    report["report_sha256"] = report_digest(report)
    return report


def rehash(report: dict) -> dict:
    report["report_sha256"] = report_digest(report)
    return report


def test_schema_3_gpu_report_is_accepted_and_older_ones_stay_valid(tmp_path: Path) -> None:
    write(tmp_path, GOOD_NAME, good_gpu_report())
    assert run(tmp_path) == 0
    write(tmp_path, GOOD_NAME, good_report())  # schema 1
    assert run(tmp_path) == 0
    no_device = good_gpu_report()
    no_device["gpu"] = {"status": "no_device", "backend": "sycl", "access": {"path": "none"},
                        "reason": "no GPU device node is visible", "devices": []}  # fmt: skip
    write(tmp_path, GOOD_NAME, rehash(no_device))
    assert run(tmp_path) == 0  # nothing measured is not a failure


def test_schema_3_requires_the_gpu_section(tmp_path: Path) -> None:
    report = good_gpu_report()
    del report["gpu"]
    write(tmp_path, GOOD_NAME, rehash(report))
    assert run(tmp_path) == 1


def test_identifying_device_facts_are_refused(tmp_path: Path) -> None:
    report = good_gpu_report()
    report["gpu"]["devices"][0]["facts"]["uuid"] = "86800be2-0000-0000-0100-000000000000"
    write(tmp_path, GOOD_NAME, rehash(report))
    assert run(tmp_path) == 1


def test_gpu_statuses_must_follow_from_the_measurements(tmp_path: Path) -> None:
    lying_device = good_gpu_report()
    lying_device["gpu"]["devices"][0]["audits"]["test_sycl_kernel_scratch"]["status"] = "fail"
    write(tmp_path, GOOD_NAME, rehash(lying_device))
    assert run(tmp_path) == 1  # device says pass, its audit failed
    lying_section = good_gpu_report()
    lying_section["gpu"]["devices"][0]["status"] = "fail"
    lying_section["gpu"]["devices"][0]["twins"]["status"] = "differing"
    write(tmp_path, GOOD_NAME, rehash(lying_section))
    assert run(tmp_path) == 1  # section says pass, a device failed
    honest = good_gpu_report()
    honest["gpu"]["status"] = "fail"
    honest["gpu"]["devices"][0]["status"] = "fail"
    honest["gpu"]["devices"][0]["twins"]["status"] = "differing"
    honest["verdict"], honest["failed_checks"] = "fail", ["gpu"]
    write(tmp_path, GOOD_NAME, rehash(honest))
    assert run(tmp_path) == 0
    lying_verdict = good_gpu_report()
    lying_verdict["gpu"] = honest["gpu"]
    write(tmp_path, GOOD_NAME, rehash(lying_verdict))
    assert run(tmp_path) == 1  # verdict pass with a failing GPU section


# ---- schema 4: library identity and the executed dispatch level ----------------------------


def library_block(pair: str = "libvmaf.so.3") -> dict:
    return {"pair": pair, "pair_source": "NEEDED names in the vmaf binary's dynamic string table",
            "version": "1.0.0", "commit": "abc1234", "build_id": "b1", "abi": "1.0.2",
            "provenance_digest": "d" * 64, "conformance_mode": "not_reported"}  # fmt: skip


def executed_section(status: str = "match") -> dict:
    return {"status": status, "reported": "neon",
            "reported_source": "host dispatch_flags (mirror of the library's CPU rules)",
            "fixtures": [{"fixture": "f", "default_simd": "neon", "scalar_simd": "scalar",
                          "status": status}]}  # fmt: skip


def v4_report(pair: str = "libvmaf.so.3", executed: dict | None = None, **sections) -> dict:
    report = good_gpu_report()
    report["schema_version"] = "4"
    report["library"] = library_block(pair)
    report["executed_dispatch"] = executed or executed_section()
    report.update(sections)
    return rehash(report)


@pytest.mark.parametrize("pair", ["libvmaf.so.3", "libvmafx.so.1", "unknown"])
def test_schema_4_reports_of_both_library_pairs_validate(tmp_path: Path, pair: str) -> None:
    write(tmp_path, GOOD_NAME, v4_report(pair))
    assert run(tmp_path) == 0


def test_schema_4_needs_its_sections_and_known_values(tmp_path: Path) -> None:
    report = v4_report()
    del report["library"]
    write(tmp_path, GOOD_NAME, rehash(report))
    assert run(tmp_path) == 1
    report = v4_report()
    del report["executed_dispatch"]
    write(tmp_path, GOOD_NAME, rehash(report))
    assert run(tmp_path) == 1
    write(tmp_path, GOOD_NAME, v4_report("libvmaf.so.2"))  # a pair no release ships
    assert run(tmp_path) == 1


def test_a_mismatched_executed_level_cannot_pass(tmp_path: Path) -> None:
    for status in ("mismatch", "not_reported"):
        write(tmp_path, GOOD_NAME, v4_report(executed=executed_section(status)))
        assert run(tmp_path) == 1
    failing = v4_report(executed=executed_section("mismatch"), verdict="fail",
                        failed_checks=["executed_dispatch"])  # fmt: skip
    write(tmp_path, GOOD_NAME, failing)
    assert run(tmp_path) == 0  # an honest failing report is accepted as a record
