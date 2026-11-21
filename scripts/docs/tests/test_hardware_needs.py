#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The hardware-we-need table: status from reports, and the refusals of the generator."""

import importlib.util
import json
import sys
import unittest
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location(
    "generate_hardware_reports", ROOT / "scripts/docs/generate-hardware-reports.py"
)
assert SPEC is not None and SPEC.loader is not None
GEN = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = GEN
SPEC.loader.exec_module(GEN)


PASSING_SECTIONS = {
    "dispatch_equivalence": "identical",
    "reference_equivalence": "identical",
    "metal_equivalence": "not_applicable",
    "metal_gate": "not_applicable",
    "unit_tests": "pass",
    "golden_gate": "not_applicable",
    "gpu": "pass",
}


def host(**kw: Any) -> dict[str, Any]:
    """A passing report of the given host: every section passes."""
    base = {
        "platform": "linux",
        "machine": "x86_64",
        "dispatch_flags": ["avx2", "avx512"],
        "cpu_model": "Intel(R) Xeon(R) Gold 6338",
    }
    report: dict[str, Any] = {"host": {**base, **kw}, "verdict": "pass", "failed_checks": []}
    report.update({key: {"status": status} for key, status in PASSING_SECTIONS.items()})
    report["image"] = {"files_match_build": True}
    return report


def gpu_report(
    family: str, status: str = "pass", rows: str = "pass", backend: str = "cuda"
) -> dict[str, Any]:
    device = {"facts": {"family": family}, "status": status, "rows": {"status": rows}}
    return {**host(), "gpu": {"backend": backend, "devices": [device]}}


def row(family_part: str) -> dict[str, Any]:
    needs: dict[str, Any] = json.loads(GEN.NEEDS.read_text(encoding="utf-8"))
    found: dict[str, Any] = next(r for r in needs["rows"] if family_part in r["family"])
    return found


class StatusTests(unittest.TestCase):
    def test_no_report(self) -> None:
        self.assertEqual(GEN._status(row("Hopper"), []), "no report yet")

    def test_cpu_report_counts_for_matching_row_only(self) -> None:
        reports = [host()]
        self.assertIn("1 reported, worst pass", GEN._status(row("Intel with AVX-512"), reports))
        self.assertEqual(
            GEN._status(row("AMD with AVX-512"), reports), "covered by Ryzen 9 9950X3D (Zen 5)"
        )
        self.assertEqual(GEN._status(row("Arm with SVE2"), reports), "no report yet")

    def test_windows_report_counts_for_its_architecture_and_the_cpu_rows(self) -> None:
        reports = [host(platform="windows")]
        self.assertIn("1 reported", GEN._status(row("Windows on x64"), reports))
        self.assertEqual(GEN._status(row("Windows on Arm"), reports), "no report yet")
        self.assertIn("1 reported", GEN._status(row("Intel with AVX-512"), reports))
        self.assertEqual(GEN._status(row("Windows on x64"), [host()]), "no report yet")
        arm = [host(platform="windows", machine="aarch64", dispatch_flags=["neon"])]
        self.assertIn("1 reported", GEN._status(row("Windows on Arm"), arm))

    def test_a_windows_cuda_report_counts_for_the_windows_gpu_row(self) -> None:
        windows = gpu_report("ampere")
        windows["host"] = {**windows["host"], "platform": "windows"}
        self.assertIn("1 reported", GEN._status(row("NVIDIA GPU under native Windows"), [windows]))
        self.assertIn("1 reported", GEN._status(row("NVIDIA Ampere"), [windows]))
        linux = [gpu_report("ampere")]
        self.assertEqual(
            GEN._status(row("NVIDIA GPU under native Windows"), linux), "no report yet"
        )

    def test_a_windows_sycl_report_counts_for_the_windows_intel_row(self) -> None:
        windows = gpu_report("xe-lp", backend="sycl")
        windows["host"] = {**windows["host"], "platform": "windows"}
        self.assertIn("1 reported", GEN._status(row("Intel GPU under native Windows"), [windows]))
        self.assertIn("1 reported", GEN._status(row("Intel Xe-LP and Xe-LPG"), [windows]))
        linux = [gpu_report("xe-lp", backend="sycl")]
        self.assertEqual(GEN._status(row("Intel GPU under native Windows"), linux), "no report yet")

    def test_gpu_report_counts_for_its_family_and_a_fail_wins(self) -> None:
        reports = [gpu_report("hopper"), gpu_report("hopper", rows="fail")]
        self.assertIn("2 reported, worst fail", GEN._status(row("Hopper"), reports))
        self.assertEqual(GEN._status(row("Blackwell"), reports), "no report yet")
        self.assertEqual(
            GEN._status(row("Hopper"), [gpu_report("hopper", backend="hip")]), "no report yet"
        )

    def test_unmeasured_rows_are_not_a_pass(self) -> None:
        status = GEN._status(row("Hopper"), [gpu_report("hopper", rows="not_measured")])
        self.assertIn("not fully measured", status)


def checked_report(failed: list[str], **host_kw: Any) -> dict[str, Any]:
    """A report whose sections pass except the named ones (section keys), with the
    verdict and failed_checks the tester would write."""
    report = host(**host_kw)
    for key in failed:
        report[key] = {"status": "fail"}
    report["verdict"] = "fail" if failed else "pass"
    report["failed_checks"] = list(failed)
    return report


class CpuRowVerdictTests(unittest.TestCase):
    """A CPU row takes the verdict of the report's CPU checks, not the report's overall
    verdict: a GPU or Metal failure is not a finding about the processor."""

    def avx2_host(self) -> dict[str, Any]:
        return {"dispatch_flags": ["sse2", "ssse3", "sse4.1", "avx2"],
                "cpu_model": "13th Gen Intel(R) Core(TM) i5-13500"}  # fmt: skip

    def test_gpu_failure_does_not_fail_the_cpu_row(self) -> None:
        # The shape of the UHD 770 report (#2116): every CPU check passes, the SYCL
        # section fails, so the report's verdict is `fail` with failed_checks ["gpu"].
        report = checked_report(["gpu"], **self.avx2_host())
        self.assertEqual(report["verdict"], "fail")
        status = GEN._status(row("x86 with AVX2"), [report])
        self.assertEqual(status, "1 reported, worst pass")

    def test_cpu_failure_fails_the_cpu_row(self) -> None:
        for check in ("dispatch_equivalence", "reference_equivalence", "unit_tests"):
            report = checked_report([check], **self.avx2_host())
            status = GEN._status(row("x86 with AVX2"), [report])
            self.assertEqual(status, "1 reported, worst fail", check)

    def test_image_mismatch_fails_the_cpu_row(self) -> None:
        report = checked_report([], **self.avx2_host())
        report["image"]["files_match_build"] = False
        report["verdict"], report["failed_checks"] = "fail", ["image_files_match_build"]
        self.assertEqual(GEN._status(row("x86 with AVX2"), [report]), "1 reported, worst fail")

    def test_metal_failure_fails_the_mac_row_only(self) -> None:
        # The shape of the M4 Pro report (#2118): the Metal checks fail. The native
        # macOS row closes the Metal rows, so they are its own checks.
        mac = {"platform": "darwin", "machine": "arm64", "dispatch_flags": ["neon"],
               "cpu_model": "Apple M4 Pro"}  # fmt: skip
        report = checked_report(["metal_equivalence", "metal_gate"], **mac)
        self.assertEqual(GEN._status(row("Apple M-series, native"), [report]),
                         "1 reported, worst fail")  # fmt: skip
        passing = checked_report([], **mac)
        self.assertEqual(GEN._status(row("Apple M-series, native"), [passing]),
                         "1 reported, worst pass")  # fmt: skip

    def test_executed_dispatch_mismatch_fails_the_cpu_row(self) -> None:
        # Schema 4: the level the library reports it executed is part of the dispatch
        # check (hw_report.EXECUTED_CHECK) and the report writer counts it, so a
        # mismatch must fail the CPU row instead of reading pass against the report's
        # own fail verdict (review of PR #3443).
        report = checked_report([], **self.avx2_host())
        report["executed_dispatch"] = {"status": "mismatch"}
        report["verdict"], report["failed_checks"] = "fail", ["executed_dispatch"]
        self.assertEqual(GEN._status(row("x86 with AVX2"), [report]), "1 reported, worst fail")

    def test_executed_dispatch_absent_or_matching_keeps_the_verdict(self) -> None:
        # A schema-3 report has no executed_dispatch section (the in-tree reports
        # carry schema 3), so it is not counted; a matching schema-4 section keeps
        # the pass.
        report = checked_report([], **self.avx2_host())
        self.assertNotIn("executed_dispatch", report)
        self.assertEqual(GEN._status(row("x86 with AVX2"), [report]), "1 reported, worst pass")
        report["executed_dispatch"] = {"status": "match"}
        self.assertEqual(GEN._status(row("x86 with AVX2"), [report]), "1 reported, worst pass")


class DriverCellTests(unittest.TestCase):
    """The Driver column is the driver: the compile target (`code_path`, the
    cubin/sm level) is a build fact and not part of it (review of PR #3443)."""

    def test_gpu_driver_is_the_driver_version_only(self) -> None:
        report = {"host": {"kernel_release": "6.6.0"}}
        facts = {"driver_version": "576.52", "code_path": "cubin sm_120"}
        self.assertEqual(GEN._driver(report, "cuda", facts), "576.52")
        self.assertNotIn("cubin", GEN._driver(report, "cuda", facts))

    def test_gpu_driver_without_a_driver_version_is_not_reported(self) -> None:
        # The shape of the shipped 5080/4060 reports: device facts carry a
        # code_path and no driver_version, so the column is "not reported" —
        # not the compile target wearing a driver label.
        facts = {"code_path": "cubin sm_89"}
        self.assertEqual(GEN._driver({"host": {}}, "cuda", facts), "not reported")

    def test_cpu_and_metal_keep_their_shapes(self) -> None:
        self.assertEqual(
            GEN._driver({"host": {"kernel_release": "10.0.26300"}}, "cpu", {}), "10.0.26300"
        )
        self.assertEqual(GEN._driver({"host": {}}, "metal", {}), "not reported")


class RefusalTests(unittest.TestCase):
    def needs(self) -> dict[str, Any]:
        needs: dict[str, Any] = json.loads(GEN.NEEDS.read_text(encoding="utf-8"))
        return needs

    def test_shipped_file_is_consistent(self) -> None:
        self.assertEqual(GEN.needs_problems(self.needs()), [])

    def test_missing_gpu_family_is_refused(self) -> None:
        needs = self.needs()
        needs["rows"] = [r for r in needs["rows"] if "Hopper" not in r["family"]]
        self.assertIn(
            "cuda family hopper has no row in hardware-needs.json", GEN.needs_problems(needs)
        )

    def test_unknown_state_id_is_refused(self) -> None:
        needs = self.needs()
        needs["rows"][0]["state"] = ["T-NO-SUCH-ROW-2026-01-01"]
        self.assertIn(
            "state id T-NO-SUCH-ROW-2026-01-01 is not in docs/state.md", GEN.needs_problems(needs)
        )

    def test_splice_needs_markers(self) -> None:
        with self.assertRaises(ValueError):
            GEN.splice("no markers", "table")
        out = GEN.splice(f"a\n{GEN.BEGIN}\nold\n{GEN.END}\nz", "T\n")
        self.assertIn("T\n", out)
        self.assertNotIn("old", out)


if __name__ == "__main__":
    unittest.main()
