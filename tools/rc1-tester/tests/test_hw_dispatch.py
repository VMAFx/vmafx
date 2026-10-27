# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for the executed dispatch level and the library identity (schema 4)."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaf_rc1_tester import hw_dispatch, hw_equiv, hw_facts, hw_report
from vmaf_rc1_tester.safe_process import CommandResult

FIXTURE = {"id": "f1", "ref": "r.yuv", "dis": "d.yuv", "width": 16, "height": 16,
           "pixel_format": "420", "bitdepth": 8}  # fmt: skip


def provenance(simd: str, **extra) -> dict:
    record = {"abi_major": 1, "abi_minor": 0, "abi_patch": 2, "version": "1.0.0",
              "commit": "abc1234", "build_id": "b1", "simd": simd, "digest": "d" * 64}  # fmt: skip
    record.update(extra)
    return record


def runner_with(by_mask: dict, with_provenance: bool = True):
    def run(argv, **_kwargs):
        mask = int(argv[argv.index("--cpumask") + 1]) if "--cpumask" in argv else None
        document = {"frames": [{"frameNum": 0, "metrics": {"vmaf": 90.0}}]}
        if with_provenance:
            document["provenance"] = provenance(by_mask[mask])
        Path(argv[argv.index("--output") + 1]).write_text(json.dumps(document))
        return CommandResult(0, "", "")

    return run


def test_reported_level_picks_the_highest_flag() -> None:
    assert hw_dispatch.reported_level(["sse2", "avx2", "avx512"]) == "avx512"
    assert hw_dispatch.reported_level(["sse2", "avx512icl", "avx512", "avx2"]) == "avx512icl"
    assert hw_dispatch.reported_level(["neon", "sve2"]) == "sve2"
    assert hw_dispatch.reported_level([]) == "scalar"  # boundary


def test_provenance_facts_reads_the_library_record() -> None:
    facts = hw_dispatch.provenance_facts({"provenance": provenance("avx2")})
    assert facts["simd"] == "avx2" and facts["abi"] == "1.0.2" and facts["digest"] == "d" * 64
    assert "conformance_mode" not in facts
    mode = hw_dispatch.provenance_facts(
        {"provenance": provenance("avx2", conformance_mode="reference-exact")}
    )
    assert mode["conformance_mode"] == "reference-exact"
    assert hw_dispatch.provenance_facts({"frames": []}) is None  # negative
    assert hw_dispatch.provenance_facts({"provenance": {"simd": 3}}) is None


def test_dispatch_cells_carry_the_executed_level() -> None:
    run = runner_with({None: "avx2", hw_equiv.SCALAR_CPUMASK: "scalar"})
    section, _ = hw_equiv.run_dispatch_equivalence("vmaf", [FIXTURE], timeout_seconds=5, runner=run)
    executed = section["fixtures"][0]["executed"]
    assert (executed["default_simd"], executed["scalar_simd"]) == ("avx2", "scalar")
    assert executed["provenance_digest"] == "d" * 64


def test_cells_without_a_provenance_record_report_no_level() -> None:
    run = runner_with({None: "avx2", hw_equiv.SCALAR_CPUMASK: "scalar"}, with_provenance=False)
    section, _ = hw_equiv.run_dispatch_equivalence("vmaf", [FIXTURE], timeout_seconds=5, runner=run)
    assert section["fixtures"][0]["executed"] == {}


def section_for(default: str, scalar: str, flags: list[str]) -> dict:
    run = runner_with({None: default, hw_equiv.SCALAR_CPUMASK: scalar})
    dispatch, _ = hw_equiv.run_dispatch_equivalence(
        "vmaf", [FIXTURE], timeout_seconds=5, runner=run
    )
    return hw_dispatch.executed_dispatch(dispatch, flags)


def test_executed_level_matching_the_host_flags_matches() -> None:
    section = section_for("avx2", "scalar", ["sse2", "avx2"])
    assert section["status"] == "match" and section["reported"] == "avx2"


def test_planted_mismatch_between_reported_and_executed_level_fails() -> None:
    # The host flags promise AVX-512; the library ran AVX2.
    section = section_for("avx2", "scalar", ["sse2", "avx2", "avx512"])
    assert section["status"] == "mismatch"
    assert section["fixtures"][0]["status"] == "mismatch"
    # The masked run must be scalar.
    assert section_for("avx2", "avx2", ["avx2"])["status"] == "mismatch"
    report = {**VERDICT_BASE, "executed_dispatch": section}
    assert hw_report.verdict_of(report, []) == ("fail", ["executed_dispatch"])


def test_missing_provenance_is_not_passing() -> None:
    run = runner_with({None: "avx2", hw_equiv.SCALAR_CPUMASK: "scalar"}, with_provenance=False)
    dispatch, _ = hw_equiv.run_dispatch_equivalence(
        "vmaf", [FIXTURE], timeout_seconds=5, runner=run
    )
    section = hw_dispatch.executed_dispatch(dispatch, ["avx2"])
    assert section["status"] == "not_reported"
    report = {**VERDICT_BASE, "executed_dispatch": section}
    assert hw_report.verdict_of(report, [])[0] == "fail"
    assert hw_report.verdict_of(report, ["dispatch"])[0] == "incomplete"  # skipped with dispatch


def test_a_skipped_dispatch_run_is_not_run() -> None:
    assert hw_dispatch.executed_dispatch({"status": "not_run"}, [])["status"] == "not_run"


VERDICT_BASE = {
    "image": {"files_match_build": True},
    "dispatch_equivalence": {"status": "identical"},
    "reference_equivalence": {"status": "identical"},
    "metal_equivalence": {"status": "no_device"},
    "metal_gate": {"status": "no_device"},
    "unit_tests": {"status": "pass"},
    "golden_gate": {"status": "pass"},
    "gpu": {"status": "not_applicable"},
}


def test_old_reports_without_the_section_keep_their_verdict() -> None:
    assert hw_report.verdict_of(VERDICT_BASE, []) == ("pass", [])
    good = {**VERDICT_BASE, "executed_dispatch": {"status": "match"}}
    assert hw_report.verdict_of(good, []) == ("pass", [])


def fake_elf(tmp_path: Path, *names: str) -> Path:
    path = tmp_path / "vmaf"
    path.write_bytes(
        b"\x7fELF" + b"".join(b"\0" + n.encode() + b"\0" for n in names) + b"\0libc.so.6\0"
    )
    return path


@pytest.mark.parametrize("name", ["libvmafx.so.1", "libvmaf.so.3"])
def test_library_pair_is_read_from_the_binary(tmp_path: Path, name: str) -> None:
    assert hw_dispatch.library_pair(fake_elf(tmp_path, name))["pair"] == name


def test_library_pair_unknown_when_absent_ambiguous_or_unreadable(tmp_path: Path) -> None:
    assert hw_dispatch.library_pair(fake_elf(tmp_path))["pair"] == "unknown"
    both = fake_elf(tmp_path, "libvmafx.so.1", "libvmaf.so.3")
    assert hw_dispatch.library_pair(both)["pair"] == "unknown"
    assert hw_dispatch.library_pair(tmp_path / "missing")["pair"] == "unknown"


def test_library_block_states_what_the_library_did_not_say(tmp_path: Path) -> None:
    run = runner_with({None: "avx2", hw_equiv.SCALAR_CPUMASK: "scalar"})
    dispatch, _ = hw_equiv.run_dispatch_equivalence(
        "vmaf", [FIXTURE], timeout_seconds=5, runner=run
    )
    block = hw_dispatch.library_block(fake_elf(tmp_path, "libvmaf.so.3"), dispatch)
    assert block["pair"] == "libvmaf.so.3"
    assert block["provenance_digest"] == "d" * 64 and block["abi"] == "1.0.2"
    assert block["conformance_mode"] == "not_reported"


def test_avx512icl_needs_its_extra_flags_and_avx2_needs_fma_bmi() -> None:
    avx512 = {"sse2", "ssse3", "sse4_1", "avx2", "fma", "bmi1", "bmi2",
              "avx512f", "avx512bw", "avx512vl", "avx512dq", "avx512cd"}  # fmt: skip
    icl_extra = {"avx512ifma", "avx512vbmi", "avx512_vbmi2", "gfni", "vaes", "vpclmulqdq",
                 "avx512_vnni", "avx512_bitalg", "avx512_vpopcntdq"}  # fmt: skip
    assert "avx512icl" not in hw_facts.x86_dispatch_flags(avx512)
    assert "avx512icl" in hw_facts.x86_dispatch_flags(avx512 | icl_extra)
    assert "avx512icl" not in hw_facts.x86_dispatch_flags((avx512 | icl_extra) - {"gfni"})
    assert "avx2" not in hw_facts.x86_dispatch_flags({"sse2", "ssse3", "sse4_1", "avx2"})
    assert "avx512" not in hw_facts.x86_dispatch_flags(avx512 - {"avx2"})
