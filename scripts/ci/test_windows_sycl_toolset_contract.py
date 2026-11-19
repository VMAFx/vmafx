#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Contract test for the Windows MSVC+SYCL oneAPI toolchain pin in libvmaf-build-matrix.yml."""

from __future__ import annotations

import re
import sys
import unittest
from copy import deepcopy
from pathlib import Path
from typing import Any

import yaml  # type: ignore[import-untyped]

REPO_ROOT = Path(__file__).resolve().parents[2]
WORKFLOW_PATH = REPO_ROOT / ".github" / "workflows" / "libvmaf-build-matrix.yml"

WINDOWS_GPU_JOB_ID = "windows-gpu-build"
ONEAPI_STEP_NAME = "Install Intel oneAPI (windows)"
MSVC_STEP_NAME = "Setup MSVC dev environment"
EXPECTED_MSVC_ACTION = "TheMrMilchmann/setup-msvc-dev@368ef7d1ee4d1171b31d4a7f67f4d954f903f5a9"
EXPECTED_WINDOWS_BASEKIT_URL = (
    "https://registrationcenter-download.intel.com/akdlm/IRC_NAS/"
    "728c4418-1b40-47a9-ad51-31530fb27c94/"
    "intel-oneapi-toolkit-2026.2.0.661_offline.exe"
)
OLD_WINDOWS_BASEKIT_URL = (
    "https://registrationcenter-download.intel.com/akdlm/IRC_NAS/"
    "1f18901e-877d-469d-a41a-a10f11b39336/"
    "intel-oneapi-base-toolkit-2025.3.0.372_offline.exe"
)
EXPECTED_DPCPP_COMPONENT = "intel.oneapi.win.cpp-dpcpp-common"
EXPECTED_AOT_REFUSED = "tgllp,adl-s,adl-p,adl-n,rpl-s,rpl-p"
EXPECTED_SYCL_STEP_IF = "steps.leg.outputs.run == 'true' && matrix.backend == 'sycl'"
APPROVED_STEP_CONDITIONS = {
    "steps.leg.outputs.run == 'true'",
    EXPECTED_SYCL_STEP_IF,
    "steps.leg.outputs.run == 'true' && matrix.backend == 'cuda'",
    "steps.leg.outputs.run == 'true' && runner.os == 'Windows'",
}
EXPECTED_INSTALLER_SIZE = "2402678488"
EXPECTED_INSTALLER_SHA256 = "e5774d5435835b66b16c4369a6c9704720fe2df9d67cdc48922c93782c152ee0"
BANNED_STL_MISMATCH_MACRO = "_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH"
MIN_ONEAPI_MAJOR = 2026

MAX_MATRIX_ROWS = 16
MAX_JOB_STEPS = 64
MAX_SCRIPT_LINES = 128
ONEAPI_URL_VERSION_RE = re.compile(
    r"intel-oneapi-(?:base-)?toolkit-(?P<major>\d+)\.(?P<minor>\d+)\.(?P<patch>\d+)\.(?P<build>\d+)_offline\.exe$"
)


def _load_windows_gpu_job(workflow_text: str) -> dict[str, Any]:
    """Parse workflow YAML and return the windows-gpu-build job mapping."""
    if BANNED_STL_MISMATCH_MACRO in workflow_text:
        raise AssertionError(f"workflow must not use unsupported {BANNED_STL_MISMATCH_MACRO} macro")
    parsed = yaml.safe_load(workflow_text)
    if not isinstance(parsed, dict):
        raise AssertionError("workflow YAML must parse to a mapping")
    jobs = parsed.get("jobs")
    if not isinstance(jobs, dict) or WINDOWS_GPU_JOB_ID not in jobs:
        raise AssertionError(f"workflow missing job '{WINDOWS_GPU_JOB_ID}'")
    job = jobs[WINDOWS_GPU_JOB_ID]
    if not isinstance(job, dict):
        raise AssertionError(f"job '{WINDOWS_GPU_JOB_ID}' must be a mapping")
    return job


def _validate_matrix_backends(job: dict[str, Any]) -> None:
    """Verify windows-gpu-build matrix declares both cuda and sycl legs without a cuda-only pin."""
    include = job.get("strategy", {}).get("matrix", {}).get("include")
    if not isinstance(include, list) or not include:
        raise AssertionError(f"job '{WINDOWS_GPU_JOB_ID}' missing strategy.matrix.include")
    if len(include) > MAX_MATRIX_ROWS:
        raise AssertionError("matrix exceeds row limit")
    backends: set[str] = set()
    for row in include[:MAX_MATRIX_ROWS]:
        if not isinstance(row, dict):
            continue
        backend = str(row.get("backend", ""))
        backends.add(backend)
        if backend == "cuda" and (
            "WINDOWS_BASEKIT_URL" in row or "oneapi" in str(row).lower() or "toolset" in row
        ):
            raise AssertionError("CUDA matrix row must not carry the SYCL toolchain pin")
    if backends != {"cuda", "sycl"}:
        raise AssertionError(f"expected matrix backends {{'cuda', 'sycl'}}, found {backends}")


def _validate_step_if_conditions(steps: list[dict[str, Any]]) -> None:
    """Ensure every post-checkout step in windows-gpu-build keeps its explicit if: guard."""
    for idx, step in enumerate(steps[:MAX_JOB_STEPS]):
        uses = str(step.get("uses", ""))
        name = str(step.get("name", f"step[{idx}]"))
        if idx == 0 and step.get("id") == "leg":
            continue
        if uses.startswith("actions/checkout@"):
            continue
        condition = step.get("if")
        if not isinstance(condition, str) or condition not in APPROVED_STEP_CONDITIONS:
            raise AssertionError(f"step '{name}' must keep its steps.leg.outputs.run if: guard")


def _validate_msvc_step(steps: list[dict[str, Any]]) -> None:
    """Verify the setup-msvc-dev step remains SHA-pinned at v4.1.0 with arch: x64."""
    msvc_steps = [s for s in steps[:MAX_JOB_STEPS] if s.get("name") == MSVC_STEP_NAME]
    if len(msvc_steps) != 1:
        raise AssertionError(f"expected exactly one '{MSVC_STEP_NAME}' step")
    step = msvc_steps[0]
    if step.get("uses") != EXPECTED_MSVC_ACTION:
        raise AssertionError(f"'{MSVC_STEP_NAME}' must stay pinned to {EXPECTED_MSVC_ACTION}")
    with_map = step.get("with")
    if not isinstance(with_map, dict) or with_map.get("arch") != "x64":
        raise AssertionError(f"'{MSVC_STEP_NAME}' must specify with.arch: x64")


def _validate_oneapi_step(steps: list[dict[str, Any]]) -> None:
    """Verify the SYCL leg pins the oneAPI 2026.2.0.661 offline installer."""
    oneapi_steps = [s for s in steps[:MAX_JOB_STEPS] if s.get("name") == ONEAPI_STEP_NAME]
    if len(oneapi_steps) != 1:
        raise AssertionError(f"expected exactly one '{ONEAPI_STEP_NAME}' step")
    step = oneapi_steps[0]
    if step.get("if") != EXPECTED_SYCL_STEP_IF:
        raise AssertionError(
            f"'{ONEAPI_STEP_NAME}' must run on the SYCL leg ({EXPECTED_SYCL_STEP_IF!r}), "
            f"found {step.get('if')!r}"
        )
    env = step.get("env")
    if not isinstance(env, dict):
        raise AssertionError(f"'{ONEAPI_STEP_NAME}' must define an env mapping")
    url = str(env.get("WINDOWS_BASEKIT_URL", ""))
    if not url:
        raise AssertionError(f"'{ONEAPI_STEP_NAME}' missing WINDOWS_BASEKIT_URL pin")
    match = ONEAPI_URL_VERSION_RE.search(url)
    if match is None or int(match.group("major")) < MIN_ONEAPI_MAJOR:
        raise AssertionError(
            f"WINDOWS_BASEKIT_URL must pin oneAPI >= {MIN_ONEAPI_MAJOR} (MSVC 14.52 STL1000, ADR-3567), "
            f"found {url!r}"
        )
    if url != EXPECTED_WINDOWS_BASEKIT_URL:
        raise AssertionError(
            f"WINDOWS_BASEKIT_URL must equal {EXPECTED_WINDOWS_BASEKIT_URL!r}, found {url!r}"
        )
    if env.get("WINDOWS_DPCPP_COMPONENTS") != EXPECTED_DPCPP_COMPONENT:
        raise AssertionError(f"WINDOWS_DPCPP_COMPONENTS must equal {EXPECTED_DPCPP_COMPONENT!r}")
    if env.get("WINDOWS_AOT_EXPECT_REFUSED") != EXPECTED_AOT_REFUSED:
        raise AssertionError(f"WINDOWS_AOT_EXPECT_REFUSED must equal {EXPECTED_AOT_REFUSED!r}")
    _validate_oneapi_script(env, str(step.get("run", "")))


def _validate_oneapi_script(env: dict[str, Any], run_script: str) -> None:
    lines = run_script.splitlines()
    if len(lines) > MAX_SCRIPT_LINES:
        raise AssertionError("installer script exceeds line limit")
    bootstrap = [
        line.strip()
        for line in lines[:MAX_SCRIPT_LINES]
        if line.strip().startswith("webimage_extracted\\bootstrapper.exe ")
    ]
    if len(bootstrap) != 1 or "-p=NEED_VS2026_INTEGRATION=0" not in bootstrap[0].split():
        raise AssertionError(
            f"'{ONEAPI_STEP_NAME}' must pass -p=NEED_VS2026_INTEGRATION=0 on VS 2026 runners"
        )
    if "exit /b %installer_exit_code%" not in run_script:
        raise AssertionError(f"'{ONEAPI_STEP_NAME}' must propagate %installer_exit_code%")
    _validate_installer_integrity(env, run_script)


def _validate_installer_integrity(env: dict[str, Any], script: str) -> None:
    if str(env.get("WINDOWS_BASEKIT_SIZE", "")) != EXPECTED_INSTALLER_SIZE:
        raise AssertionError("installer size pin missing or changed")
    if env.get("WINDOWS_BASEKIT_SHA256") != EXPECTED_INSTALLER_SHA256:
        raise AssertionError("installer SHA-256 pin missing or changed")
    lines = [line.strip() for line in script.splitlines()[:MAX_SCRIPT_LINES] if line.strip()]
    checks = (
        "curl.exe --fail --output %TEMP%\\webimage.exe --url %WINDOWS_BASEKIT_URL%",
        "powershell.exe -NoProfile -Command \"$ErrorActionPreference='Stop'; if ((Get-Item (Join-Path $env:TEMP 'webimage.exe')).Length -ne [int64]$env:WINDOWS_BASEKIT_SIZE) { exit 1 }\"",
        "powershell.exe -NoProfile -Command \"$ErrorActionPreference='Stop'; if ((Get-FileHash -Algorithm SHA256 (Join-Path $env:TEMP 'webimage.exe')).Hash -ne $env:WINDOWS_BASEKIT_SHA256) { exit 1 }\"",
        "start /b /wait %TEMP%\\webimage.exe -s -x",
    )
    positions = [
        next((i for i, line in enumerate(lines) if line.startswith(check)), -1) for check in checks
    ]
    if -1 in positions or positions != sorted(set(positions)):
        raise AssertionError("installer download, size and hash checks must precede extraction")
    if any(lines[i + 1] != "if errorlevel 1 exit /b 1" for i in positions[:3]):
        raise AssertionError("installer download and checks must fail closed")


def validate_windows_sycl_toolset_contract(workflow_text: str) -> None:
    """Validate the Windows MSVC+SYCL toolchain pin and step guards in libvmaf-build-matrix.yml."""
    job = _load_windows_gpu_job(workflow_text)
    _validate_matrix_backends(job)
    raw_steps = job.get("steps")
    if not isinstance(raw_steps, list) or not raw_steps:
        raise AssertionError(f"job '{WINDOWS_GPU_JOB_ID}' has no steps")
    if len(raw_steps) > MAX_JOB_STEPS:
        raise AssertionError("job exceeds step limit")
    steps = [s for s in raw_steps[:MAX_JOB_STEPS] if isinstance(s, dict)]
    _validate_step_if_conditions(steps)
    _validate_msvc_step(steps)
    _validate_oneapi_step(steps)
    _validate_aot_probe(steps)


def _validate_aot_probe(steps: list[dict[str, Any]]) -> None:
    probes = [(i, s) for i, s in enumerate(steps) if s.get("id") == "aot_targets"]
    configs = [(i, s) for i, s in enumerate(steps) if s.get("name") == "Configure libvmaf (SYCL)"]
    if len(probes) != 1 or len(configs) != 1 or probes[0][0] >= configs[0][0]:
        raise AssertionError("AOT probe must precede SYCL configure")
    probe, config = probes[0][1], configs[0][1]
    if probe.get("if") != EXPECTED_SYCL_STEP_IF or config.get("if") != EXPECTED_SYCL_STEP_IF:
        raise AssertionError("AOT probe and configure must run on the SYCL leg")
    if "scripts\\ci\\ocloc_aot_targets.py" not in str(probe.get("run", "")):
        raise AssertionError("AOT probe script missing")
    if "-Dsycl_icpx_aot_targets=${{ steps.aot_targets.outputs.targets }}" not in str(
        config.get("run", "")
    ):
        raise AssertionError("SYCL configure must use AOT probe output")


class WindowsSyclToolsetContractTest(unittest.TestCase):
    """Positive, negative, and boundary contract tests for Windows MSVC+SYCL toolchain pin."""

    def test_planted_bounds_and_guard_rejected(self) -> None:
        workflow = yaml.safe_load(WORKFLOW_PATH.read_text(encoding="utf-8"))
        for kind in ("rows", "steps", "guard"):
            planted = deepcopy(workflow)
            job = planted["jobs"][WINDOWS_GPU_JOB_ID]
            if kind == "rows":
                job["strategy"]["matrix"]["include"].extend([{"backend": "sycl"}] * MAX_MATRIX_ROWS)
            elif kind == "steps":
                job["steps"].extend(
                    [{"name": "extra", "if": "steps.leg.outputs.run == 'true'"}] * MAX_JOB_STEPS
                )
            else:
                guarded = next(s for s in job["steps"] if isinstance(s.get("if"), str))
                guarded["if"] += " || always()"
            with self.subTest(kind=kind), self.assertRaises(AssertionError):
                validate_windows_sycl_toolset_contract(yaml.safe_dump(planted))

    def test_planted_switch_only_in_comment_rejected(self) -> None:
        text = WORKFLOW_PATH.read_text(encoding="utf-8")
        switch = "-p=NEED_VS2026_INTEGRATION=0"
        planted = text.replace(switch, "", 1).replace(
            "          webimage_extracted\\bootstrapper.exe",
            f"          rem {switch}\n" "          webimage_extracted\\bootstrapper.exe",
            1,
        )
        with self.assertRaises(AssertionError):
            validate_windows_sycl_toolset_contract(planted)

    def test_planted_installer_integrity_failures(self) -> None:
        workflow = yaml.safe_load(WORKFLOW_PATH.read_text(encoding="utf-8"))
        for kind in ("size", "sha256", "late_size", "late_sha256", "curl", "exit", "length"):
            planted = deepcopy(workflow)
            target = next(
                s
                for s in planted["jobs"][WINDOWS_GPU_JOB_ID]["steps"]
                if s.get("name") == ONEAPI_STEP_NAME
            )
            if kind in ("size", "sha256"):
                del target["env"]["WINDOWS_BASEKIT_" + kind.upper()]
            elif kind == "curl":
                target["run"] = target["run"].replace("--fail", "", 1)
            elif kind == "exit":
                target["run"] = target["run"].replace("if errorlevel 1 exit /b 1", "rem ignored", 1)
            elif kind == "length":
                target["run"] += "\nrem extra" * MAX_SCRIPT_LINES
            else:
                marker = "WINDOWS_BASEKIT_SIZE" if kind == "late_size" else "Get-FileHash"
                lines = target["run"].splitlines()
                line = next(line for line in lines if marker in line)
                lines.remove(line)
                lines.append(line)
                target["run"] = "\n".join(lines)
            with self.subTest(kind=kind), self.assertRaises(AssertionError):
                validate_windows_sycl_toolset_contract(yaml.safe_dump(planted))

    def test_live_workflow_satisfies_windows_sycl_toolset_contract(self) -> None:
        workflow_text = WORKFLOW_PATH.read_text(encoding="utf-8")
        validate_windows_sycl_toolset_contract(workflow_text)

    def test_planted_refused_targets_rejected(self) -> None:
        text = WORKFLOW_PATH.read_text(encoding="utf-8")
        for planted in ("tgllp", EXPECTED_AOT_REFUSED + ",bmg-g31", ""):
            with self.subTest(planted=planted), self.assertRaises(AssertionError):
                validate_windows_sycl_toolset_contract(text.replace(EXPECTED_AOT_REFUSED, planted))

    def assert_windows_pin_documentation(self, text: str) -> None:
        self.assertIn("lane pins its own release (2026.2.0.661, ADR-3567)", text)
        self.assertIn("tester zip keeps the build-config.env release (ADR-1566)", text)

    def test_planted_probe_contract_failures(self) -> None:
        job = _load_windows_gpu_job(WORKFLOW_PATH.read_text(encoding="utf-8"))
        steps = job["steps"]
        probe_index = next(i for i, step in enumerate(steps) if step.get("id") == "aot_targets")
        config_index = next(
            i for i, step in enumerate(steps) if step.get("name") == "Configure libvmaf (SYCL)"
        )
        missing = steps[:probe_index] + steps[probe_index + 1 :]
        late = list(steps)
        late[probe_index], late[config_index] = late[config_index], late[probe_index]
        no_output = [dict(step) for step in steps]
        no_output[config_index]["run"] = no_output[config_index]["run"].replace(
            "-Dsycl_icpx_aot_targets=${{ steps.aot_targets.outputs.targets }}", ""
        )
        for planted in (missing, late, no_output):
            with self.assertRaises(AssertionError):
                _validate_aot_probe(planted)

    def test_windows_pin_documentation(self) -> None:
        for path in ("build-config.env", "docs/development/tester-image.md"):
            self.assert_windows_pin_documentation((REPO_ROOT / path).read_text(encoding="utf-8"))

    def test_negative_planted_shared_release_rejected(self) -> None:
        for path in ("build-config.env", "docs/development/tester-image.md"):
            text = (REPO_ROOT / path).read_text(encoding="utf-8")
            for marker in (
                "lane pins its own release (2026.2.0.661, ADR-3567)",
                "tester zip keeps the build-config.env release (ADR-1566)",
            ):
                with self.assertRaises(AssertionError):
                    self.assert_windows_pin_documentation(text.replace(marker, "shared release"))

    def test_contract_registered_in_tooling_suite(self) -> None:
        sys.path.insert(0, str(REPO_ROOT))
        from scripts.ci.suite_registry import load_registry, suite_files  # noqa: PLC0415

        rel_path = Path(__file__).resolve().relative_to(REPO_ROOT).as_posix()
        registry = load_registry(REPO_ROOT)
        self.assertEqual(suite_files(registry, [rel_path], "tooling"), [rel_path])

    def test_negative_planted_old_2025_3_0_pin_rejected(self) -> None:
        workflow_text = WORKFLOW_PATH.read_text(encoding="utf-8")
        planted = workflow_text.replace(
            EXPECTED_WINDOWS_BASEKIT_URL, OLD_WINDOWS_BASEKIT_URL
        ).replace(" -p=NEED_VS2026_INTEGRATION=0", "")
        with self.assertRaises(AssertionError) as ctx:
            validate_windows_sycl_toolset_contract(planted)
        self.assertIn("oneAPI >= 2026", str(ctx.exception))

    def test_negative_planted_missing_url_pin_rejected(self) -> None:
        workflow_text = WORKFLOW_PATH.read_text(encoding="utf-8")
        planted = re.sub(r"(?m)^\s*WINDOWS_BASEKIT_URL:.*\n", "", workflow_text)
        with self.assertRaises(AssertionError) as ctx:
            validate_windows_sycl_toolset_contract(planted)
        self.assertIn("missing WINDOWS_BASEKIT_URL pin", str(ctx.exception))

    def test_negative_planted_missing_vs2026_integration_skip_rejected(self) -> None:
        workflow_text = WORKFLOW_PATH.read_text(encoding="utf-8")
        planted = workflow_text.replace(" -p=NEED_VS2026_INTEGRATION=0", "", 1)
        self.assertNotEqual(workflow_text, planted)
        with self.assertRaises(AssertionError) as ctx:
            validate_windows_sycl_toolset_contract(planted)
        self.assertIn("-p=NEED_VS2026_INTEGRATION=0", str(ctx.exception))

    def test_negative_planted_stl_mismatch_macro_rejected(self) -> None:
        workflow_text = WORKFLOW_PATH.read_text(encoding="utf-8")
        planted = workflow_text.replace(
            "-Dcpp_std=c++latest",
            f"-Dcpp_std=c++latest -D{BANNED_STL_MISMATCH_MACRO}",
            1,
        )
        with self.assertRaises(AssertionError) as ctx:
            validate_windows_sycl_toolset_contract(planted)
        self.assertIn(BANNED_STL_MISMATCH_MACRO, str(ctx.exception))

    def test_negative_planted_missing_step_if_guard_rejected(self) -> None:
        workflow_text = WORKFLOW_PATH.read_text(encoding="utf-8")
        target = f"      - name: {ONEAPI_STEP_NAME}\n" f"        if: {EXPECTED_SYCL_STEP_IF}\n"
        replacement = f"      - name: {ONEAPI_STEP_NAME}\n"
        planted = workflow_text.replace(target, replacement, 1)
        self.assertNotEqual(workflow_text, planted)
        with self.assertRaises(AssertionError) as ctx:
            validate_windows_sycl_toolset_contract(planted)
        self.assertIn("if: guard", str(ctx.exception))

    def test_boundary_pin_present_on_cuda_leg_only_rejected(self) -> None:
        workflow_text = WORKFLOW_PATH.read_text(encoding="utf-8")
        target = f"      - name: {ONEAPI_STEP_NAME}\n" f"        if: {EXPECTED_SYCL_STEP_IF}\n"
        cuda_only_step = (
            f"      - name: {ONEAPI_STEP_NAME}\n"
            "        if: steps.leg.outputs.run == 'true' && matrix.backend == 'cuda'\n"
        )
        planted_step = workflow_text.replace(target, cuda_only_step, 1)
        self.assertNotEqual(workflow_text, planted_step)
        with self.assertRaises(AssertionError) as ctx:
            validate_windows_sycl_toolset_contract(planted_step)
        self.assertIn("must run on the SYCL leg", str(ctx.exception))

        cuda_matrix_pin = workflow_text.replace(
            "          - backend: cuda\n",
            f"          - backend: cuda\n            WINDOWS_BASEKIT_URL: {EXPECTED_WINDOWS_BASEKIT_URL}\n",
            1,
        ).replace(EXPECTED_WINDOWS_BASEKIT_URL, OLD_WINDOWS_BASEKIT_URL)
        with self.assertRaises(AssertionError) as ctx2:
            validate_windows_sycl_toolset_contract(cuda_matrix_pin)
        self.assertIn("CUDA matrix row", str(ctx2.exception))


if __name__ == "__main__":
    unittest.main()
