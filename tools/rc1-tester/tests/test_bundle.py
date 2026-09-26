# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for shareable RC1 archive construction."""

from __future__ import annotations

import hashlib
import json
import math
import sys
import tarfile
import zipfile
from dataclasses import replace
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaf_rc1_tester import __version__
from vmaf_rc1_tester.bundle import create_report_bundle
from vmaf_rc1_tester.probe import (
    CPUInfo,
    DiagnosticReport,
    HardwareProbes,
    OSInfo,
    ToolchainInfo,
    VmafBinaryInfo,
)
from vmaf_rc1_tester.validate import ValidationResult

ARCHIVE_FILE_MODE = 0o644
ZIP_UNIX_SYSTEM = 3
COLLECTOR_REVISION = "f" * 40


class FakeCompleted:
    def __init__(self, stdout: str) -> None:
        self.returncode = 0
        self.stdout = stdout
        self.stderr = ""


def _git_runner(command: list[str], **_kwargs: object) -> FakeCompleted:
    assert command[0] == "git"
    assert command[-2:] == ["rev-parse", "HEAD"]
    return FakeCompleted(COLLECTOR_REVISION)


def _bundle(
    report: DiagnosticReport,
    validations: list[ValidationResult],
    *,
    dest_dir: Path,
    archive_format: str = "tar.gz",
):
    return create_report_bundle(
        report,
        validations,
        dest_dir=dest_dir,
        archive_format=archive_format,
        runner=_git_runner,
    )


def _report() -> DiagnosticReport:
    return DiagnosticReport(
        OSInfo("Linux", "6.8.0", "x86_64", "Ubuntu 24.04 LTS", False),
        CPUInfo("AMD EPYC | test", 16, True, True, False),
        ToolchainInfo("clang 18", "clang++ 18", "1.4", "1.11", "12.3", "none", "none", "none"),
        HardwareProbes(),
        VmafBinaryInfo(
            str(Path.home() / "private-build" / "vmaf"),
            True,
            "3.2.0-rc1",
            "b" * 64,
            ["cpu", "cuda", "sycl", "hip", "metal"],
        ),
    )


def _validation(
    backend: str = "cuda",
    verdict: str = "PASS",
    *,
    requested_by_user: bool = True,
) -> ValidationResult:
    return ValidationResult(
        backend_requested=backend,
        backend_observed=backend if verdict == "PASS" else None,
        requested_by_user=requested_by_user,
        device_index=None if backend == "cpu" else 0,
        environment_overrides=({"CUDA_VISIBLE_DEVICES": "0"} if backend == "cuda" else {}),
        verdict=verdict,
        process_exit_code=0 if verdict == "PASS" else 100,
        duration_seconds=0.12,
        command_argv=[str(Path.home() / "private-build" / "vmaf"), "--backend", backend],
        stdout="four frames processed",
        stderr=f"log path {Path.home() / 'private-log'}",
        json_output={"backend_used": backend} if verdict == "PASS" else None,
        binary_sha256="b" * 64,
        model_sha256="e" * 64,
        fixture_sha256={
            "reference": "c" * 64,
            "distorted": "d" * 64,
            "pinned_cpu_scores": "f" * 64,
        },
        reference_backend="cpu" if backend != "cpu" else "pinned CPU snapshot",
        parity_tolerance=5e-5,
        max_abs_score_delta=0.0 if verdict == "PASS" else None,
        notes="Explicit backend result.",
    )


def test_create_tar_contains_manifest_report_and_per_backend_evidence(tmp_path: Path) -> None:
    artifact = _bundle(_report(), [_validation("cpu"), _validation("cuda")], dest_dir=tmp_path)
    assert artifact.archive_path.is_file()
    with tarfile.open(artifact.archive_path, "r:gz") as archive:
        members = archive.getmembers()
        names = [member.name for member in members]
    assert all(member.uid == 0 and member.gid == 0 for member in members)
    assert all(member.uname == "" and member.gname == "" for member in members)
    assert all(member.mode == ARCHIVE_FILE_MODE and member.mtime == 0 for member in members)
    for suffix in ("manifest.json", "report.md", "diagnostics.json", "SHA256SUMS"):
        assert any(name.endswith(suffix) for name in names)
    assert any(name.endswith("smoke_cpu.json") for name in names)
    assert any(name.endswith("smoke_cuda.log") for name in names)


def test_create_zip_contains_report(tmp_path: Path) -> None:
    artifact = _bundle(_report(), [_validation()], dest_dir=tmp_path, archive_format="zip")
    with zipfile.ZipFile(artifact.archive_path) as archive:
        members = archive.infolist()
        assert any(member.filename.endswith("report.md") for member in members)
    assert all(member.date_time == (1980, 1, 1, 0, 0, 0) for member in members)
    assert all(member.create_system == ZIP_UNIX_SYSTEM for member in members)
    assert all((member.external_attr >> 16) & 0o777 == ARCHIVE_FILE_MODE for member in members)


def test_manifest_checksums_match_written_files(tmp_path: Path) -> None:
    artifact = _bundle(_report(), [_validation()], dest_dir=tmp_path)
    manifest = json.loads(artifact.manifest_path.read_text(encoding="utf-8"))
    assert manifest["schema_version"] == "1.0"
    assert manifest["tool_version"] == __version__
    assert manifest["collector_checkout_revision"] == COLLECTOR_REVISION
    assert manifest["validation_verdicts"] == {"cuda": "PASS"}
    assert "manifest.json" not in manifest["files"]
    checksum_lines = (artifact.staging_dir / "SHA256SUMS").read_text().splitlines()
    checksum_names = {line.split("  ", 1)[1] for line in checksum_lines}
    assert checksum_names == set(manifest["files"]) - {"SHA256SUMS"}
    assert "manifest.json" not in checksum_names
    assert "SHA256SUMS" not in checksum_names
    for filename, metadata in manifest["files"].items():
        path = artifact.staging_dir / filename
        assert hashlib.sha256(path.read_bytes()).hexdigest() == metadata["sha256"]
        assert path.stat().st_size == metadata["size_bytes"]
    assert artifact.archive_sha256 == hashlib.sha256(artifact.archive_path.read_bytes()).hexdigest()


def test_bundle_redacts_home_paths_and_escapes_markdown_cells(tmp_path: Path) -> None:
    artifact = _bundle(_report(), [_validation()], dest_dir=tmp_path)
    report = artifact.report_path.read_text(encoding="utf-8")
    diagnostics = (artifact.staging_dir / "diagnostics.json").read_text(encoding="utf-8")
    log = (artifact.staging_dir / "smoke_cuda.log").read_text(encoding="utf-8")
    assert str(Path.home()) not in report + diagnostics + log
    assert "$HOME/private-build/vmaf" in report
    assert "AMD EPYC \\| test" in report


def test_bundle_records_command_binary_and_fixture_provenance(tmp_path: Path) -> None:
    artifact = _bundle(_report(), [_validation()], dest_dir=tmp_path)
    diagnostics = json.loads((artifact.staging_dir / "diagnostics.json").read_text())
    validation = diagnostics["validations"][0]
    assert validation["command_argv"][-2:] == ["--backend", "cuda"]
    assert validation["device_index"] == 0
    assert validation["environment_overrides"] == {"CUDA_VISIBLE_DEVICES": "0"}
    assert validation["requested_by_user"] is True
    assert validation["reference_backend"] == "cpu"
    assert validation["parity_tolerance"] == 5e-5
    assert validation["max_abs_score_delta"] == 0.0
    assert validation["binary_sha256"] == "b" * 64
    assert validation["model_sha256"] == "e" * 64
    assert validation["fixture_sha256"] == {
        "reference": "c" * 64,
        "distorted": "d" * 64,
        "pinned_cpu_scores": "f" * 64,
    }


def test_report_explains_four_frame_correctness_and_dispatch_limit(tmp_path: Path) -> None:
    artifact = _bundle(_report(), [_validation()], dest_dir=tmp_path)
    report = artifact.report_path.read_text(encoding="utf-8")
    assert "not proof that every individual feature ran on the accelerator" in report
    assert "Frame 3 exercises temporal motion" in report
    assert "ADR-0214 bound for listed feature metrics" in report
    assert "adopted by ADR-1342 for overall VMAF" in report
    assert "RC2 benchmarking" in report


def test_bundle_rejects_empty_validation_set(tmp_path: Path) -> None:
    with pytest.raises(ValueError, match="at least one"):
        _bundle(_report(), [], dest_dir=tmp_path)


def test_bundle_serializes_failed_nonfinite_evidence_as_strict_json(tmp_path: Path) -> None:
    poisoned = replace(
        _validation(verdict="FAIL"),
        json_output={"backend_used": "cuda", "frames": [{"metrics": {"vmaf": math.nan}}]},
    )
    artifact = _bundle(_report(), [poisoned], dest_dir=tmp_path)
    diagnostics_text = (artifact.staging_dir / "diagnostics.json").read_text()
    smoke_text = (artifact.staging_dir / "smoke_cuda.json").read_text()
    assert "NaN" not in diagnostics_text + smoke_text
    assert json.loads(smoke_text)["frames"][0]["metrics"]["vmaf"] is None
