# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Linux release manifest and planted glibc symbol compatibility cases."""

from __future__ import annotations

import importlib.util
import re
import subprocess  # nosec B404 -- subprocess is used only for mocked CompletedProcess and CalledProcessError fixtures and does not execute commands
import tomllib
from pathlib import Path
from unittest.mock import patch

import pytest
import yaml  # type: ignore[import-untyped]  # no PyYAML stubs in the hook environment

from scripts.ci import check_hosted_runner_labels
from scripts.release import check_glibc_floor, distribution

ROOT = Path(__file__).resolve().parents[3]
OBJDUMP_TIMEOUT_S = 30
ARGPARSE_USAGE_EXIT = 2
LIBRARY_COUNT = 2


@pytest.mark.parametrize(  # type: ignore[untyped-decorator]  # pytest is untyped for the mypy gate (--no-site-packages)
    "version",
    ["2.2.5", "2.17", "2.28"],
)
def test_manifest_glibc_floor_accepts_compatible_symbols(version: str) -> None:
    symbols = f"0000 DF *UND* 0000 (GLIBC_{version}) memcpy\n"
    assert check_glibc_floor.check_symbols(symbols, (2, 28)) is None


@pytest.mark.parametrize(  # type: ignore[untyped-decorator]  # pytest is untyped for the mypy gate (--no-site-packages)
    "version",
    ["2.29", "2.41", "2.100", "PRIVATE", "ABI_DT_RELR"],
)
def test_manifest_glibc_floor_rejects_planted_new_symbols(version: str) -> None:
    symbols = f"0000 DF *UND* 0000 (GLIBC_{version}) memcpy\n"
    assert check_glibc_floor.check_symbols(symbols, (2, 28)) is not None


@pytest.mark.parametrize(  # type: ignore[untyped-decorator]  # pytest is untyped for the mypy gate (--no-site-packages)
    "version",
    ["3.4", "3.4.19", "3.4.25"],
)
def test_manifest_glibcxx_floor_accepts_compatible_symbols(version: str) -> None:
    symbols = (
        f"0000 DF *UND* 0000 (GLIBC_2.17) memcpy\n0000 DF *UND* 0000 (GLIBCXX_{version}) _ZdaPv\n"
    )
    assert check_glibc_floor.check_symbols(symbols, (2, 28), (3, 4, 25)) is None


@pytest.mark.parametrize(  # type: ignore[untyped-decorator]  # pytest is untyped for the mypy gate (--no-site-packages)
    "version",
    ["3.4.26", "3.4.30", "3.5", "PRIVATE", "DEBUG"],
)
def test_manifest_glibcxx_floor_rejects_planted_new_symbols(version: str) -> None:
    symbols = (
        f"0000 DF *UND* 0000 (GLIBC_2.17) memcpy\n0000 DF *UND* 0000 (GLIBCXX_{version}) _ZdaPv\n"
    )
    assert check_glibc_floor.check_symbols(symbols, (2, 28), (3, 4, 25)) is not None


def test_manifest_glibc_floor_rejects_empty_symbol_table() -> None:
    assert check_glibc_floor.check_symbols("", (2, 28)) is not None


def test_manifest_glibc_floor_objdump_failure_is_fatal(tmp_path: Path) -> None:
    binary = tmp_path / "vmaf"
    binary.write_bytes(b"ELF fixture")
    with patch("scripts.release.check_glibc_floor.subprocess.run") as run:
        run.side_effect = subprocess.CalledProcessError(1, ["objdump"], stderr="bad ELF")
        assert check_glibc_floor.main(["2.28", str(binary)]) == 1


def test_manifest_glibc_floor_accepts_staged_cli() -> None:
    with patch("scripts.release.check_glibc_floor.subprocess.run") as run:
        run.return_value = subprocess.CompletedProcess(
            [], 0, "(GLIBC_2.28) memcpy\n(GLIBCXX_3.4.25) _ZdaPv", ""
        )
        assert check_glibc_floor.main(["2.28", "--cxx-floor", "3.4.25", "artifacts/vmaf"]) == 0
        assert run.call_args.kwargs["timeout"] == OBJDUMP_TIMEOUT_S


def test_manifest_glibc_floor_rejects_newer_glibcxx_in_staged_cli() -> None:
    with patch("scripts.release.check_glibc_floor.subprocess.run") as run:
        run.return_value = subprocess.CompletedProcess(
            [], 0, "(GLIBC_2.28) memcpy\n(GLIBCXX_3.4.30) _ZdaPv", ""
        )
        assert check_glibc_floor.main(["2.28", "--cxx-floor", "3.4.25", "artifacts/vmaf"]) == 1


def test_manifest_glibc_floor_rejects_invalid_floor() -> None:
    with pytest.raises(SystemExit) as error:
        check_glibc_floor.main(["", "artifacts/vmaf"])
    assert error.value.code == ARGPARSE_USAGE_EXIT


def test_manifest_glibc_floor_inspects_each_library(tmp_path: Path) -> None:
    binaries = [tmp_path / "vmaf", tmp_path / "libvmafx.so.1"]
    for binary in binaries:
        binary.write_bytes(b"ELF fixture")
    with patch("scripts.release.check_glibc_floor.subprocess.run") as run:
        run.side_effect = [
            subprocess.CompletedProcess([], 0, "(GLIBC_2.17) memcpy", ""),
            subprocess.CompletedProcess([], 0, "(GLIBC_2.41) sqrtf", ""),
        ]
        assert check_glibc_floor.main(["2.28", *(str(p) for p in binaries)]) == 1
        assert run.call_count == LIBRARY_COUNT


def test_manifest_native_linux_architectures_share_release_channels() -> None:
    manifest = tomllib.loads((ROOT / "packaging/manifest.toml").read_text())
    assert distribution.check(ROOT, manifest) == []

    # Every channel expected to provide native Linux downloads must include both architectures
    expected_channels = ["github-release", "integrator-pack"]
    for channel_name in expected_channels:
        channel_artifacts = manifest["channels"][channel_name]["artifacts"]
        assert "native-linux-x86_64" in channel_artifacts
        assert "native-linux-arm64" in channel_artifacts

    for channel in manifest["channels"].values():
        has_x86 = "native-linux-x86_64" in channel["artifacts"]
        has_arm = "native-linux-arm64" in channel["artifacts"]
        assert has_x86 == has_arm

    arm = manifest["artifacts"]["native-linux-arm64"]
    assert arm["platform"] == "linux-arm64"
    assert arm["signing"] == "sigstore"
    assert arm["sbom"] == "vmafx.spdx.json"
    assert arm["files"] == ["vmafx-linux-arm64.tar.gz"]


def test_manifest_rejects_channel_missing_x86_architecture() -> None:
    manifest = tomllib.loads((ROOT / "packaging/manifest.toml").read_text())
    manifest["channels"]["github-release"]["artifacts"].remove("native-linux-x86_64")
    with pytest.raises(AssertionError):
        for channel_name in ["github-release", "integrator-pack"]:
            channel_artifacts = manifest["channels"][channel_name]["artifacts"]
            assert "native-linux-x86_64" in channel_artifacts
            assert "native-linux-arm64" in channel_artifacts


def test_manifest_rejects_channel_missing_arm_architecture() -> None:
    manifest = tomllib.loads((ROOT / "packaging/manifest.toml").read_text())
    manifest["channels"]["github-release"]["artifacts"].remove("native-linux-arm64")
    with pytest.raises(AssertionError):
        for channel_name in ["github-release", "integrator-pack"]:
            channel_artifacts = manifest["channels"][channel_name]["artifacts"]
            assert "native-linux-x86_64" in channel_artifacts
            assert "native-linux-arm64" in channel_artifacts


def test_manifest_native_linux_workflow_builds_and_checks_both_architectures() -> None:
    workflow = yaml.safe_load((ROOT / ".github/workflows/supply-chain.yml").read_text())
    build = workflow["jobs"]["build-artifacts"]
    legs = build["strategy"]["matrix"]["include"]
    assert {leg["arch"] for leg in legs} == {"x86_64", "arm64"}
    assert next(leg for leg in legs if leg["arch"] == "arm64")["runner"] == "ubuntu-26.04-arm"

    floor_check_pattern = re.compile(
        r"^\s*python3\s+scripts/release/check_glibc_floor\.py\s+"
        r'.*"\$floor".*artifacts/vmaf\s+artifacts/lib\*\.so\*',
        re.MULTILINE | re.DOTALL,
    )
    floor_steps = [
        step for step in build["steps"] if floor_check_pattern.search(step.get("run", ""))
    ]
    assert len(floor_steps) == 1
    run_text = floor_steps[0]["run"]
    assert "NATIVE_GLIBC_FLOOR" in run_text
    assert "NATIVE_GLIBCXX_FLOOR" in run_text


def test_manifest_rejects_workflow_with_commented_or_echoed_floor_check() -> None:
    workflow = yaml.safe_load((ROOT / ".github/workflows/supply-chain.yml").read_text())
    build = workflow["jobs"]["build-artifacts"]
    floor_check_pattern = re.compile(
        r"^\s*python3\s+scripts/release/check_glibc_floor\.py\s+"
        r'.*"\$floor".*artifacts/vmaf\s+artifacts/lib\*\.so\*',
        re.MULTILINE | re.DOTALL,
    )
    # Plant an echo instead of command execution
    for step in build["steps"]:
        if "check_glibc_floor.py" in step.get("run", ""):
            step["run"] = step["run"].replace(
                "python3 scripts/release/check_glibc_floor.py",
                "echo python3 scripts/release/check_glibc_floor.py",
            )
    assert not any(floor_check_pattern.search(s.get("run", "")) for s in build["steps"])


def test_manifest_rejects_workflow_without_floor_check_step() -> None:
    workflow = yaml.safe_load((ROOT / ".github/workflows/supply-chain.yml").read_text())
    build = workflow["jobs"]["build-artifacts"]
    floor_check_pattern = re.compile(
        r"^\s*python3\s+scripts/release/check_glibc_floor\.py\s+"
        r'.*"\$floor".*artifacts/vmaf\s+artifacts/lib\*\.so\*',
        re.MULTILINE | re.DOTALL,
    )
    # Remove the floor check step completely
    build["steps"] = [s for s in build["steps"] if "check_glibc_floor.py" not in s.get("run", "")]
    assert not any(floor_check_pattern.search(s.get("run", "")) for s in build["steps"])


def test_manifest_native_builder_allows_only_the_architecture_pin_argument() -> None:
    path = ROOT / "scripts/ci/check-dev-container-build-secret.py"
    spec = importlib.util.spec_from_file_location("native_builder_contract", path)
    assert spec is not None and spec.loader is not None
    checker = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(checker)
    builder = (ROOT / "scripts/ci/build-dev-container-stage.sh").read_text()
    containerfile = (ROOT / "dev/Containerfile").read_text()
    assert checker.validate_stage_builder(builder, containerfile) == []
    planted = builder.replace("docker build \\", "docker build --build-arg FOO=bar \\", 1)
    assert checker.validate_stage_builder(planted, containerfile)


def test_manifest_native_runner_targets_match_the_workflow() -> None:
    checker = check_hosted_runner_labels
    workflow = yaml.safe_load((ROOT / ".github/workflows/supply-chain.yml").read_text())
    labels = checker.job_labels(workflow)
    table = {("supply-chain.yml", job): values for job, values in labels.items()}
    assert checker.check_labels(table, set()) == []


def test_manifest_native_builder_pins_match_their_mirrors() -> None:
    settings = dict(
        re.findall(
            r'^([A-Z0-9_]+)="([^"\n]*)"$',
            (ROOT / "build-config.env").read_text(),
            re.MULTILINE,
        )
    )
    containerfile = (ROOT / "dev/Containerfile").read_text()
    defaults = dict(re.findall(r'^ARG ([A-Z0-9_]+)="?([^"\n]*)"?$', containerfile, re.MULTILINE))
    for name in ("NATIVE_RELEASE_BUILDER_BASE", "NATIVE_NASM_VERSION", "NATIVE_NASM_SHA256"):
        assert settings[name] == defaults[name]
    assert settings["NATIVE_GLIBC_FLOOR"] == "2.28"
    assert settings["NATIVE_GLIBCXX_FLOOR"] == "3.4.25"
    for name, arch in (
        ("NATIVE_RELEASE_BUILDER_BASE", "x86_64"),
        ("NATIVE_RELEASE_BUILDER_ARM64", "aarch64"),
    ):
        assert re.fullmatch(
            rf"quay.io/pypa/manylinux_2_28_{arch}@sha256:[a-f0-9]{{64}}", settings[name]
        )
