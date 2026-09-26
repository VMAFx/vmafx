# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for honest CLI dispatch and exit semantics."""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaf_rc1_tester.cli import (
    EXIT_BACKEND_UNAVAILABLE,
    EXIT_FAILURE,
    EXIT_INCOMPLETE,
    _result_exit_code,
    main,
)
from vmaf_rc1_tester.probe import (
    AcceleratorDevice,
    CPUInfo,
    DiagnosticReport,
    HardwareProbes,
    OSInfo,
    ToolchainInfo,
    VmafBinaryInfo,
)
from vmaf_rc1_tester.validate import ValidationResult


def _report() -> DiagnosticReport:
    return DiagnosticReport(
        OSInfo("Linux", "6.8", "x86_64", "Test Linux", False),
        CPUInfo("Test CPU", 8, True, False, False),
        ToolchainInfo(*(["not installed"] * 8)),
        HardwareProbes(
            cuda_devices=[AcceleratorDevice("cuda", "Test GPU", ordinal=1)],
        ),
        VmafBinaryInfo("/opt/vmaf", True, "3.2.0", "a" * 64, ["cpu", "cuda"]),
    )


def _result(
    backend: str,
    verdict: str = "PASS",
    *,
    requested_by_user: bool = True,
) -> ValidationResult:
    exit_code = {
        "PASS": 0,
        "FAIL": 7,
        "SKIPPED": None,
        "BACKEND_UNAVAILABLE": 100,
        "FUTURE_VERDICT": 0,
    }[verdict]
    return ValidationResult(
        backend_requested=backend,
        backend_observed=backend if verdict == "PASS" else None,
        requested_by_user=requested_by_user,
        device_index=None if backend == "cpu" else 0,
        environment_overrides={},
        verdict=verdict,
        process_exit_code=exit_code,
        duration_seconds=0.1,
        command_argv=["/opt/vmaf", "--backend", backend],
        stdout="",
        stderr="",
        json_output={"backend_used": backend} if verdict == "PASS" else None,
        binary_sha256="a" * 64,
        model_sha256="d" * 64,
        fixture_sha256={"reference": "b" * 64, "distorted": "c" * 64},
        reference_backend="cpu" if backend != "cpu" else "pinned CPU snapshot",
        parity_tolerance=5e-5,
        max_abs_score_delta=0.0 if verdict == "PASS" else None,
        notes="test result",
    )


def _patch_probe(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr("vmaf_rc1_tester.cli.run_full_probe", lambda **_kwargs: _report())
    monkeypatch.setattr(
        "vmaf_rc1_tester.bundle._collector_checkout_revision",
        lambda runner=None: "f" * 40,
    )


def test_help_and_missing_subcommand_are_clean(capsys: pytest.CaptureFixture[str]) -> None:
    with pytest.raises(SystemExit) as help_exit:
        main(["--help"])
    assert help_exit.value.code == 0
    assert "bounded RC1 hardware" in capsys.readouterr().out
    with pytest.raises(SystemExit) as missing_exit:
        main([])
    assert missing_exit.value.code == EXIT_INCOMPLETE
    assert "required" in capsys.readouterr().err


def test_list_tools_reports_release_boundaries(capsys: pytest.CaptureFixture[str]) -> None:
    assert main(["list-tools"]) == 0
    output = capsys.readouterr().out
    assert "Repository tool inventory by release phase" in output
    assert "Performance benchmarking/tuning starts in RC2" in output


def test_probe_json_and_text(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    _patch_probe(monkeypatch)
    assert main(["probe", "--json"]) == 0
    assert '"vmaf_binary"' in capsys.readouterr().out
    assert main(["probe"]) == 0
    output = capsys.readouterr().out
    assert "VMAFx RC1 hardware and toolchain probe" in output
    assert "cuda[1]:Test GPU" in output


@pytest.mark.parametrize(
    ("verdict", "expected"),
    [("PASS", 0), ("FAIL", 1), ("SKIPPED", 2), ("BACKEND_UNAVAILABLE", 100)],
)
def test_validate_propagates_honest_status(
    verdict: str,
    expected: int,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    _patch_probe(monkeypatch)
    monkeypatch.setattr(
        "vmaf_rc1_tester.cli.run_smoke_validation",
        lambda _report, backend, **kwargs: _result(
            backend,
            verdict,
            requested_by_user=kwargs.get("requested_by_user", True),
        ),
    )
    assert main(["validate", "--backend", "cuda"]) == expected


def test_validate_deduplicates_repeated_backend(
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    _patch_probe(monkeypatch)
    calls: list[str] = []

    def validate(
        _report: DiagnosticReport,
        backend: str,
        device_index: int,
        **kwargs: object,
    ) -> ValidationResult:
        assert device_index == 4
        calls.append(backend)
        return _result(backend, requested_by_user=bool(kwargs.get("requested_by_user", True)))

    monkeypatch.setattr("vmaf_rc1_tester.cli.run_smoke_validation", validate)
    assert (
        main(
            [
                "validate",
                "--backend",
                "cpu",
                "--backend",
                "cpu",
                "--device-index",
                "4",
            ]
        )
        == 0
    )
    assert calls == ["cpu"]
    assert "cpu: PASS" in capsys.readouterr().out


def test_accelerator_request_runs_automatic_cpu_reference_first(
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    _patch_probe(monkeypatch)
    calls: list[tuple[str, bool, ValidationResult | None]] = []

    def validate(
        _report: DiagnosticReport,
        backend: str,
        device_index: int,
        *,
        cpu_reference: ValidationResult | None = None,
        requested_by_user: bool = True,
    ) -> ValidationResult:
        assert device_index == 0
        calls.append((backend, requested_by_user, cpu_reference))
        return _result(backend, requested_by_user=requested_by_user)

    monkeypatch.setattr("vmaf_rc1_tester.cli.run_smoke_validation", validate)
    assert main(["validate", "--backend", "cuda"]) == 0
    assert calls[0] == ("cpu", False, None)
    assert calls[1][0:2] == ("cuda", True)
    assert calls[1][2] is not None
    assert calls[1][2].backend_requested == "cpu"
    output = capsys.readouterr().out
    assert "cpu (automatic reference): PASS" in output
    assert "cuda: PASS" in output


def test_bundle_is_created_even_when_backend_is_unavailable(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    _patch_probe(monkeypatch)
    monkeypatch.setattr(
        "vmaf_rc1_tester.cli.run_smoke_validation",
        lambda _report, backend, **_kwargs: _result(backend, "BACKEND_UNAVAILABLE"),
    )
    rc = main(["bundle", "--backend", "hip", "--out-dir", str(tmp_path)])
    assert rc == EXIT_BACKEND_UNAVAILABLE
    assert len(list(tmp_path.glob("*.tar.gz"))) == 1


def test_auto_backend_is_rejected() -> None:
    with pytest.raises(SystemExit) as exc:
        main(["validate", "--backend", "auto"])
    assert exc.value.code == EXIT_INCOMPLETE


def test_negative_device_index_is_rejected_by_cli() -> None:
    with pytest.raises(SystemExit) as exc:
        main(["validate", "--backend", "cuda", "--device-index", "-1"])
    assert exc.value.code == EXIT_INCOMPLETE


def test_exit_reducer_fails_closed_for_empty_or_unknown_verdicts() -> None:
    assert _result_exit_code([]) == EXIT_FAILURE
    assert _result_exit_code([_result("cpu", "FUTURE_VERDICT")]) == EXIT_FAILURE
