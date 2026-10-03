# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for the tester image report: facts, equivalence, references, suites, verdict."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaf_rc1_tester import hw_equiv, hw_facts, hw_reference, hw_report, hw_suites
from vmaf_rc1_tester.safe_process import CommandResult

FIXTURE = {"id": "f1", "ref": "r.yuv", "dis": "d.yuv", "width": 16, "height": 16,
           "pixel_format": "420", "bitdepth": 8}  # fmt: skip


def vmaf_json(values: dict[str, list[float | None]]) -> str:
    frames = []
    for index in range(len(next(iter(values.values())))):
        frames.append({"frameNum": index, "metrics": {k: v[index] for k, v in values.items()}})
    return json.dumps({"frames": frames})


def fake_runner(by_mask: dict[int | None, dict[str, list[float | None]]], code: int = 0):
    """A runner that writes the vmaf JSON for the --cpumask it is given."""

    def run(argv, **_kwargs):
        mask = int(argv[argv.index("--cpumask") + 1]) if "--cpumask" in argv else None
        Path(argv[argv.index("--output") + 1]).write_text(vmaf_json(by_mask[mask]))
        return CommandResult(code, "", "boom" if code else "")

    return run


# ---- hw_facts ---------------------------------------------------------------------------


def test_arm_flags_neon_always_sve2_on_hwcap2_bit1() -> None:
    assert hw_facts.arm_dispatch_flags("aarch64", 0) == ["neon"]
    assert hw_facts.arm_dispatch_flags("aarch64", 1 << 1) == ["neon", "sve2"]
    assert hw_facts.arm_dispatch_flags("aarch64", 1 << 2) == ["neon"]  # boundary: wrong bit
    assert hw_facts.arm_dispatch_flags("aarch64", None) == ["neon"]
    assert hw_facts.arm_dispatch_flags("x86_64", 1 << 1) == []


def test_x86_avx512_needs_every_listed_flag() -> None:
    base = {"sse2", "ssse3", "sse4_1", "avx2", "avx512f", "avx512bw", "avx512vl", "avx512dq"}
    assert "avx512" not in hw_facts.x86_dispatch_flags(base)
    assert "avx512" in hw_facts.x86_dispatch_flags(base | {"avx512cd"})


def test_cpuinfo_allow_list_drops_identifiers(tmp_path: Path) -> None:
    path = tmp_path / "cpuinfo"
    path.write_text(
        "processor : 0\nmodel name : Test CPU\nSerial : 0123456789\nHardware : Board\n"
        "Features : fp asimd\nCPU part : 0x22\nmodel name : second\n"
    )
    found = hw_facts.read_cpuinfo(str(path))
    assert found == {"model name": "Test CPU", "features": "fp asimd", "cpu part": "0x22"}
    assert hw_facts.read_cpuinfo(str(tmp_path / "missing")) == {}


def test_cpu_model_falls_back_to_implementer_and_part() -> None:
    info = {"cpu implementer": "0x61", "cpu part": "0x32"}
    assert hw_facts.cpu_model_string(info) == "implementer 0x61, part 0x32"
    assert hw_facts.cpu_model_string({"model name": "X"}) == "X"


def test_host_facts_carry_no_host_identifier() -> None:
    text = json.dumps(hw_facts.collect_host_facts()).lower()
    import getpass
    import socket

    assert socket.gethostname().lower() not in text or len(socket.gethostname()) < 4
    assert getpass.getuser().lower() not in text or len(getpass.getuser()) < 4


# ---- hw_equiv ---------------------------------------------------------------------------


def test_parse_scores_positive_and_negative() -> None:
    assert hw_equiv.parse_scores(vmaf_json({"a": [1.0, None]})) == {"a": [1.0, None]}
    with pytest.raises(hw_equiv.FixtureRunError):
        hw_equiv.parse_scores("not json")
    with pytest.raises(hw_equiv.FixtureRunError):
        hw_equiv.parse_scores(json.dumps({"frames": []}))
    skipped = {"frames": [{"frameNum": 1, "metrics": {"a": 1.0}}]}
    with pytest.raises(hw_equiv.FixtureRunError):
        hw_equiv.parse_scores(json.dumps(skipped))


def test_compare_identical_and_one_bit_difference() -> None:
    same = hw_equiv.compare_scores({"a": [1.0, 2.0]}, {"a": [1.0, 2.0]})
    assert same["differing_values"] == 0 and same["values"] == 2
    one_ulp = 0.1 + 1e-17
    other = 0.1 + 2e-16
    diff = hw_equiv.compare_scores({"a": [1.0, 0.1], "b": [3.0]}, {"a": [1.0, other], "b": [3.0]})
    assert diff["differing_values"] == 1 and diff["differing_metrics"] == 1
    detail = diff["details"][0]
    assert detail["metric"] == "a" and detail["first_frame"] == 1
    assert detail["left"] == f"{0.1:.17g}" and detail["right"] == f"{other:.17g}"
    assert one_ulp == 0.1 or one_ulp != other


def test_compare_boundary_shapes_and_nulls() -> None:
    assert hw_equiv.compare_scores({"a": [None]}, {"a": [None]})["differing_values"] == 0
    assert hw_equiv.compare_scores({"a": [None]}, {"a": [0.0]})["differing_values"] == 1
    shape = hw_equiv.compare_scores({"a": [1.0]}, {"a": [1.0, 2.0]})
    assert shape["differing_metrics"] == 1
    absent = hw_equiv.compare_scores({"a": [1.0]}, {})
    assert absent["details"][0]["right"] == "absent"


def test_argv_selects_scalar_only_with_cpumask() -> None:
    default = hw_equiv.build_argv("vmaf", FIXTURE, "o.json", None)
    scalar = hw_equiv.build_argv("vmaf", FIXTURE, "o.json", hw_equiv.SCALAR_CPUMASK)
    assert "--cpumask" not in default and scalar[-2:] == ["--cpumask", "4294967295"]
    assert default.count("--feature") == len(hw_equiv.EXTRACTORS)
    assert "--precision" in default and default[default.index("--precision") + 1] == "max"


def test_dispatch_equivalence_identical_differing_and_error() -> None:
    values = {"a": [1.0, 2.0]}
    cell, raw = hw_equiv.run_dispatch_equivalence(
        "vmaf", [FIXTURE], timeout_seconds=1, runner=fake_runner({None: values, 4294967295: values})
    )
    assert cell["status"] == "identical" and "f1" in raw
    other = {"a": [1.0, 2.5]}
    cell, _ = hw_equiv.run_dispatch_equivalence(
        "vmaf", [FIXTURE], timeout_seconds=1, runner=fake_runner({None: values, 4294967295: other})
    )
    assert cell["status"] == "differing"
    assert cell["fixtures"][0]["details"][0]["first_frame"] == 1
    cell, raw = hw_equiv.run_dispatch_equivalence(
        "vmaf", [FIXTURE], timeout_seconds=1, runner=fake_runner({None: values}, code=3)
    )
    assert cell["status"] == "error" and raw == {}


# ---- hw_reference -----------------------------------------------------------------------


def write_refs(directory: Path, machine: str, scores, flags=("neon",)) -> None:
    runner = fake_runner({None: scores, 4294967295: scores})
    hw_reference.generate_reference(
        "vmaf", [FIXTURE], directory, machine=machine, flags=flags, timeout_seconds=1, runner=runner
    )


def test_reference_roundtrip_identical(tmp_path: Path) -> None:
    scores = {"a": [0.1, 2.0]}
    write_refs(tmp_path, "aarch64", scores)
    raw = {"f1": (scores, scores)}
    result = hw_reference.run_reference_equivalence(
        raw, tmp_path, machine="aarch64", host_flags=["neon"]
    )
    assert result["status"] == "identical"
    assert result["cross_arch_x86_scalar"]["status"] == "missing"  # informational only
    assert result["cross_arch_x86_scalar"]["gating"] is False


def test_planted_wrong_reference_value_fails_closed(tmp_path: Path) -> None:
    scores = {"a": [0.1, 2.0]}
    write_refs(tmp_path, "aarch64", scores)
    path = tmp_path / "aarch64-scalar.json"
    document = json.loads(path.read_text())
    document["fixtures"]["f1"]["a"][1] = 2.0000000000000004
    path.write_text(json.dumps(document))
    result = hw_reference.run_reference_equivalence(
        {"f1": (scores, scores)}, tmp_path, machine="aarch64", host_flags=["neon"]
    )
    cell = result["own_arch_scalar"]["fixtures"][0]
    assert result["status"] == "differing"
    assert cell["details"][0] == {
        "metric": "a", "differing_values": 1, "first_frame": 1,
        "left": "2", "right": "2.0000000000000004", "max_abs_diff": "4.4408920985006262e-16",
    }  # fmt: skip


def test_missing_own_arch_reference_is_not_a_pass(tmp_path: Path) -> None:
    result = hw_reference.run_reference_equivalence(
        {"f1": ({"a": [1.0]}, {"a": [1.0]})}, tmp_path, machine="aarch64", host_flags=["neon"]
    )
    assert result["status"] == "missing"


def test_default_reference_with_other_flags_is_informational(tmp_path: Path) -> None:
    scores = {"a": [1.0]}
    write_refs(tmp_path, "aarch64", scores, flags=("neon", "sve2"))
    other = {"a": [2.0]}
    result = hw_reference.run_reference_equivalence(
        {"f1": (other, scores)}, tmp_path, machine="aarch64", host_flags=["neon"]
    )
    cell = result["own_arch_default"]
    assert cell["comparable"] is False and cell["gating"] is False
    assert cell["status"] == "differing" and result["status"] == "identical"


def test_load_reference_rejects_malformed(tmp_path: Path) -> None:
    for text in ("", "[]", '{"schema": 2, "fixtures": {}}', '{"schema": 1}'):
        (tmp_path / "x.json").write_text(text)
        assert hw_reference.load_reference(tmp_path / "x.json") is None
    assert hw_reference.load_reference(tmp_path / "absent.json") is None
    assert hw_reference.overall_status([]) == "missing"


# ---- hw_suites --------------------------------------------------------------------------


def manifest(tmp_path: Path, names: list[str]) -> Path:
    path = tmp_path / "unit-tests.json"
    path.write_text(json.dumps({"tests": [{"name": n, "cmd": f"/t/{n}"} for n in names]}))
    return path


def test_unit_tests_pass_skip_fail(tmp_path: Path) -> None:
    codes = {"/t/a": 0, "/t/b": 77, "/t/c": 1}

    def run(argv, **_kwargs):
        return CommandResult(codes[argv[0]], "", "")

    result = hw_suites.run_unit_tests(
        manifest(tmp_path, ["a", "b", "c"]), timeout_seconds=1, runner=run
    )
    assert (result["passed"], result["skipped"], result["failed"]) == (1, 1, 1)
    assert result["failures"] == ["c"] and result["status"] == "fail"
    only_skips = hw_suites.run_unit_tests(manifest(tmp_path, ["b"]), timeout_seconds=1, runner=run)
    assert only_skips["status"] == "fail"  # nothing ran: not a pass


def test_unit_tests_timeout_counts_as_failure_and_bad_manifest(tmp_path: Path) -> None:
    def run(argv, **_kwargs):
        raise TimeoutError("slow")

    result = hw_suites.run_unit_tests(manifest(tmp_path, ["a"]), timeout_seconds=1, runner=run)
    assert result["failed"] == 1 and result["status"] == "fail"
    missing = hw_suites.run_unit_tests(tmp_path / "none.json", timeout_seconds=1)
    assert missing["status"] == "not_run" and "unreadable" in missing["reason"]


def test_parse_junit_counts(tmp_path: Path) -> None:
    path = tmp_path / "j.xml"
    path.write_text(
        "<testsuites><testsuite>"
        '<testcase classname="m.T" name="ok"/>'
        '<testcase classname="m.T" name="bad"><failure/></testcase>'
        '<testcase classname="m.T" name="err"><error/></testcase>'
        '<testcase classname="m.T" name="skip"><skipped/></testcase>'
        "</testsuite></testsuites>"
    )
    result = hw_suites.parse_junit(path)
    assert (result["passed"], result["failed"], result["skipped"]) == (1, 2, 1)
    assert result["failures"] == ["m.T::bad", "m.T::err"]


def test_golden_gate_without_python_tests_is_not_run(tmp_path: Path) -> None:
    result = hw_suites.run_golden_gate(tmp_path, timeout_seconds=1)
    assert result["status"] == "not_run"


# ---- hw_report --------------------------------------------------------------------------


def minimal_report(**overrides) -> dict:
    section = {"status": "identical"}
    suite = {"status": "pass", "total": 1, "passed": 1, "failed": 0, "skipped": 0, "failures": []}
    report = {
        "image": {"files_match_build": True},
        "dispatch_equivalence": section,
        "reference_equivalence": section,
        "metal_equivalence": {"status": "no_device"},
        "metal_gate": {"status": "no_device"},
        "unit_tests": suite,
        "golden_gate": suite,
    }
    report.update(overrides)
    return report


def test_verdict_pass_fail_incomplete() -> None:
    assert hw_report.verdict_of(minimal_report(), []) == ("pass", [])
    assert hw_report.verdict_of(minimal_report(), ["golden"]) == ("incomplete", [])
    bad = minimal_report(dispatch_equivalence={"status": "differing"})
    assert hw_report.verdict_of(bad, []) == ("fail", ["dispatch_equivalence"])
    failing = minimal_report(unit_tests={"status": "fail"}, image={"files_match_build": False})
    assert hw_report.verdict_of(failing, [])[1] == ["unit_tests", "image_files_match_build"]


def test_digest_ignores_note_and_hash_only() -> None:
    report = {"a": 1, "note": "x", "report_sha256": "y"}
    base = hw_report.report_digest(report)
    assert hw_report.report_digest({**report, "note": "other", "report_sha256": "z"}) == base
    assert hw_report.report_digest({**report, "a": 2}) != base


def test_metal_and_golden_applicability_in_verdict() -> None:
    report = minimal_report(golden_gate={"status": "not_applicable"})
    assert hw_report.verdict_of(report, []) == ("pass", [])
    report = minimal_report(metal_equivalence={"status": "differing"})
    assert hw_report.verdict_of(report, []) == ("fail", ["metal_equivalence"])
    report = minimal_report(metal_equivalence={"status": "error"})
    assert hw_report.verdict_of(report, [])[0] == "fail"
    # ADR-1496: the parity gate's Metal cells are a check of their own.
    report = minimal_report(metal_gate={"status": "fail"})
    assert hw_report.verdict_of(report, []) == ("fail", ["metal_gate"])
    report = minimal_report(metal_gate={"status": "not_applicable"})
    assert hw_report.verdict_of(report, []) == ("pass", [])


def test_not_exercised_names_sve2_only_when_missing() -> None:
    def items(host, skipped=(), na=None):
        report = {"host": host, "metal_equivalence": {"status": "identical"}}
        return [e["item"] for e in hw_report.not_exercised(report, skipped, na or {})]

    host = {"platform": "linux", "machine": "aarch64", "dispatch_flags": ["neon"]}
    assert "SVE2 kernels" in items(host) and "Metal" in items(host)
    host = {"platform": "linux", "machine": "aarch64", "dispatch_flags": ["neon", "sve2"]}
    assert "SVE2 kernels" not in items(host)
    assert "golden" in items(host, ["golden"])
    mac = {"platform": "darwin", "machine": "arm64", "dispatch_flags": ["neon"]}
    assert "Metal" not in items(mac)
    assert "golden check" in items(mac, na={"golden": "no python"})


def fake_image(root: Path, *, matching: bool) -> None:
    """A minimal image root: vmaf stub, library stub, build-info with their hashes."""
    (root / "image").mkdir()
    (root / "image" / "fixtures.json").write_text('{"fixtures": []}')
    vmaf = root / "build" / "tools" / "vmaf"
    vmaf.parent.mkdir(parents=True)
    vmaf.write_text("#!/bin/sh\necho 1.0.0-test\n")
    vmaf.chmod(0o755)
    lib = root / "build" / "src" / "libvmaf.so.3.0.0"
    lib.parent.mkdir(parents=True)
    lib.write_bytes(b"lib")
    info = {
        "vmaf_sha256": hw_report.sha256_file(vmaf),
        "libvmaf_sha256": hw_report.sha256_file(lib),
    }
    if not matching:
        info["vmaf_sha256"] = "0" * 64
    (root / "image" / "build-info.json").write_text(json.dumps(info))


def skip_all_args(root: Path) -> list[str]:
    args = ["--image-root", str(root), "--note", "n" * 600]
    for check in hw_report.CHECKS:
        args += ["--skip", check]
    return args


def test_main_everything_skipped_is_incomplete_exit_2(tmp_path, capsys) -> None:
    fake_image(tmp_path, matching=True)
    code = hw_report.main(skip_all_args(tmp_path))
    out = capsys.readouterr()
    report = json.loads(out.out)
    assert code == 2 and report["verdict"] == "incomplete"
    assert report["image"]["files_match_build"] is True
    assert report["image"]["vmaf_version"] == "1.0.0-test"
    assert len(report["note"]) == 500
    assert report["report_sha256"] == hw_report.report_digest(report)
    assert "verdict incomplete" in out.err
    assert list(report)[:3] == ["schema_version", "tool", "generated_utc"]


def test_main_fails_closed_when_binary_differs_from_build(tmp_path, capsys) -> None:
    fake_image(tmp_path, matching=False)
    code = hw_report.main(skip_all_args(tmp_path))
    report = json.loads(capsys.readouterr().out)
    assert code == 1 and report["verdict"] == "fail"
    assert report["failed_checks"] == ["image_files_match_build"]


def test_suggested_file_name_slug_and_boundary() -> None:
    def name(model: str) -> str:
        host = {"cpu_model": model}
        return hw_report.suggested_file_name(
            {"host": host, "generated_utc": "2026-10-03T10:00:00Z"}
        )

    assert name("Apple M4 (Max)") == "docs/hardware-reports/2026-10-03-apple-m4-max.json"
    assert name("!!!") == "docs/hardware-reports/2026-10-03-cpu.json"
    assert name("x" * 200).endswith("-" + "x" * 60 + ".json")


def test_golden_environment_offline_and_venv_packages(tmp_path: Path) -> None:
    env = hw_suites.golden_environment(tmp_path, tmp_path / "build", tmp_path / "ws")
    assert env["VMAF_FORCE_BACKEND"] == "cpu" and env["CUDA_VISIBLE_DEVICES"] == ""
    assert env["PYTHONPATH"].split(":")[0] == str(tmp_path / "python")
    assert all(p in env["PYTHONPATH"] for p in __import__("site").getsitepackages())
    assert env["VMAF_WORKSPACE"] == str(tmp_path / "ws") and env["HOME"] == str(tmp_path / "ws")
    assert env["LD_LIBRARY_PATH"] == f"{tmp_path / 'build' / 'src'}:{tmp_path / 'lib'}"
