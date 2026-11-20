#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Contract tests for the shared oneAPI toolchain pin in both Windows SYCL workflows."""

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
TESTER_WORKFLOW_PATH = REPO_ROOT / ".github" / "workflows" / "windows-tester-bundle.yml"
BUILD_CONFIG_PATH = REPO_ROOT / "build-config.env"

WINDOWS_GPU_JOB_ID = "windows-gpu-build"
ONEAPI_STEP_NAME = "Install Intel oneAPI (windows)"
TESTER_ONEAPI_STEP = "Install Intel oneAPI (pinned offline installer)"
TESTER_BUILD_STEP = "Build the SYCL zip"
TESTER_SYCL_IF = "matrix.gpu == 'sycl'"
LOAD_CONFIG = 'scripts/ci/load-build-config.sh >> "$GITHUB_ENV"'
SKIP_VS2026 = "-p=NEED_VS2026_INTEGRATION=0"
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
EXPECTED_ONEAPI_VERSION = "2026.2.0.661"
WINDOWS_PINS = {
    "ONEAPI_WINDOWS_VERSION": EXPECTED_ONEAPI_VERSION,
    "INTEL_BASEKIT_WINDOWS_URL": EXPECTED_WINDOWS_BASEKIT_URL,
    "INTEL_BASEKIT_WINDOWS_SIZE": EXPECTED_INSTALLER_SIZE,
    "INTEL_BASEKIT_WINDOWS_SHA256": EXPECTED_INSTALLER_SHA256,
    "ONEAPI_WINDOWS_AOT_EXPECT_REFUSED": EXPECTED_AOT_REFUSED,
}
WINDOWS_INLINE_PIN_RE = re.compile(
    r'(?m)^[ \t]*(?:set "?)?(?:INTEL_BASEKIT_WINDOWS_(?:URL|SIZE|SHA256)'
    r"|ONEAPI_WINDOWS_(?:VERSION|AOT_EXPECT_REFUSED)|WINDOWS_BASEKIT_(?:URL|SIZE|SHA256))"
    r"[ \t]*[:=]"
)
AOT_OUTPUT = "${{ steps.aot_targets.outputs.targets }}"
SETVARS = r'call "C:\Program Files (x86)\Intel\oneAPI\setvars.bat"'
BANNED_STL_MISMATCH_MACRO = "_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH"
MIN_ONEAPI_MAJOR = 2026

MAX_MATRIX_ROWS = 16
MAX_JOB_STEPS = 64
MAX_SCRIPT_LINES = 128
ONEAPI_URL_VERSION_RE = re.compile(
    r"intel-oneapi-(?:base-)?toolkit-(?P<major>\d+)\.(?P<minor>\d+)\.(?P<patch>\d+)\.(?P<build>\d+)_offline\.exe$"
)


def _load_job(workflow_text: str, job_id: str = WINDOWS_GPU_JOB_ID) -> dict[str, Any]:
    """Parse workflow YAML and return the requested job mapping."""
    if BANNED_STL_MISMATCH_MACRO in workflow_text:
        raise AssertionError(f"workflow must not use unsupported {BANNED_STL_MISMATCH_MACRO} macro")
    parsed = yaml.safe_load(workflow_text)
    if not isinstance(parsed, dict):
        raise AssertionError("workflow YAML must parse to a mapping")
    jobs = parsed.get("jobs")
    if not isinstance(jobs, dict) or job_id not in jobs:
        raise AssertionError(f"workflow missing job '{job_id}'")
    job = jobs[job_id]
    if not isinstance(job, dict):
        raise AssertionError(f"job '{job_id}' must be a mapping")
    return job


def _job_steps(job: dict[str, Any]) -> list[dict[str, Any]]:
    steps = job.get("steps")
    if not isinstance(steps, list) or not steps:
        raise AssertionError("job has no steps")
    if len(steps) > MAX_JOB_STEPS:
        raise AssertionError("job exceeds step limit")
    return [step for step in steps if isinstance(step, dict)]


def _step_index(steps: list[dict[str, Any]], value: str, key: str = "name") -> int:
    matches = [i for i, step in enumerate(steps[:MAX_JOB_STEPS]) if step.get(key) == value]
    if len(matches) != 1:
        raise AssertionError(f"expected exactly one step with {key}={value!r}")
    return matches[0]


def _validate_build_config(text: str) -> None:
    for key, expected in WINDOWS_PINS.items():
        assignments = re.findall(rf"(?m)^{key}=([^\n]*)$", text)
        if not assignments:
            raise AssertionError(f"missing {key} pin")
        if key == "INTEL_BASEKIT_WINDOWS_URL":
            match = ONEAPI_URL_VERSION_RE.search(assignments[0].strip('"'))
            if match is None or int(match.group("major")) < MIN_ONEAPI_MAJOR:
                raise AssertionError(f"{key} must pin oneAPI >= {MIN_ONEAPI_MAJOR} (ADR-3568)")
        if assignments != [f'"{expected}"']:
            raise AssertionError(f"{key} must be pinned once to {expected!r}")


def _validate_shared_pin_documentation(text: str) -> None:
    prose = " ".join(re.sub(r"(?m)^# *", "", text).split())
    if not re.search(
        r"Windows MSVC\+SYCL CI leg and (?:the )?Windows SYCL tester zip share", prose
    ):
        raise AssertionError("shared Windows pin documentation must name both Windows SYCL legs")
    if EXPECTED_ONEAPI_VERSION not in prose or "ADR-3568" not in prose:
        raise AssertionError("shared Windows pin documentation must name the release and ADR-3568")


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
            WINDOWS_INLINE_PIN_RE.search("\n".join(f"{key}:" for key in row))
            or "oneapi" in str(row).lower()
            or "toolset" in row
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
    """Verify the SYCL installer consumes and verifies the shared pins."""
    index = _step_index(steps, ONEAPI_STEP_NAME)
    step = steps[index]
    if step.get("if") != EXPECTED_SYCL_STEP_IF:
        raise AssertionError(
            f"'{ONEAPI_STEP_NAME}' must run on the SYCL leg ({EXPECTED_SYCL_STEP_IF!r}), "
            f"found {step.get('if')!r}"
        )
    _validate_config_loader(steps, index, EXPECTED_SYCL_STEP_IF)
    if step.get("env", {}).get("WINDOWS_DPCPP_COMPONENTS") != EXPECTED_DPCPP_COMPONENT:
        raise AssertionError(f"WINDOWS_DPCPP_COMPONENTS must equal {EXPECTED_DPCPP_COMPONENT!r}")
    _validate_oneapi_script(str(step.get("run", "")))


def _validate_config_loader(steps: list[dict[str, Any]], installer: int, condition: str) -> None:
    index = _step_index(steps, "Load build config")
    step = steps[index]
    if index >= installer or step.get("if") != condition:
        raise AssertionError("Load build config must precede oneAPI install on the SYCL leg")
    if step.get("shell") != "bash" or str(step.get("run", "")).strip() != LOAD_CONFIG:
        raise AssertionError("Load build config must export the shared pins to GITHUB_ENV")


def _validate_oneapi_script(run_script: str) -> None:
    lines = run_script.splitlines()
    if len(lines) > MAX_SCRIPT_LINES:
        raise AssertionError("installer script exceeds line limit")
    bootstrap = [
        line.strip()
        for line in lines[:MAX_SCRIPT_LINES]
        if line.strip().startswith("webimage_extracted\\bootstrapper.exe ")
    ]
    if len(bootstrap) != 1 or SKIP_VS2026 not in bootstrap[0].split():
        raise AssertionError(
            f"'{ONEAPI_STEP_NAME}' must pass -p=NEED_VS2026_INTEGRATION=0 on VS 2026 runners"
        )
    if "exit /b %installer_exit_code%" not in run_script:
        raise AssertionError(f"'{ONEAPI_STEP_NAME}' must propagate %installer_exit_code%")
    if 'echo expected_refused=%ONEAPI_WINDOWS_AOT_EXPECT_REFUSED%>>"%GITHUB_OUTPUT%"' not in [
        line.strip() for line in lines
    ]:
        raise AssertionError("oneAPI install must export the shared refused AOT targets")
    _validate_installer_integrity(run_script)


def _validate_installer_integrity(script: str) -> None:
    lines = [line.strip() for line in script.splitlines()[:MAX_SCRIPT_LINES] if line.strip()]
    checks = (
        "curl.exe --fail --output %TEMP%\\webimage.exe --url %INTEL_BASEKIT_WINDOWS_URL%",
        "powershell.exe -NoProfile -Command \"$ErrorActionPreference='Stop'; if ((Get-Item (Join-Path $env:TEMP 'webimage.exe')).Length -ne [int64]$env:INTEL_BASEKIT_WINDOWS_SIZE) { exit 1 }\"",
        "powershell.exe -NoProfile -Command \"$ErrorActionPreference='Stop'; if ((Get-FileHash -Algorithm SHA256 (Join-Path $env:TEMP 'webimage.exe')).Hash -ne $env:INTEL_BASEKIT_WINDOWS_SHA256) { exit 1 }\"",
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
    job = _load_job(workflow_text)
    _validate_matrix_backends(job)
    _validate_no_inline_pin(workflow_text)
    steps = _job_steps(job)
    _validate_step_if_conditions(steps)
    _validate_msvc_step(steps)
    _validate_oneapi_step(steps)
    _validate_aot_probe(steps)


def _validate_no_inline_pin(workflow_text: str) -> None:
    if WINDOWS_INLINE_PIN_RE.search(workflow_text) or any(
        value in workflow_text for value in WINDOWS_PINS.values()
    ):
        raise AssertionError("Windows oneAPI pin must come from build-config.env")


def _validate_aot_probe(steps: list[dict[str, Any]]) -> None:
    probes = [(i, s) for i, s in enumerate(steps) if s.get("id") == "aot_targets"]
    configs = [(i, s) for i, s in enumerate(steps) if s.get("name") == "Configure libvmaf (SYCL)"]
    if len(probes) != 1 or len(configs) != 1 or probes[0][0] >= configs[0][0]:
        raise AssertionError("AOT probe must precede SYCL configure")
    probe, config = probes[0][1], configs[0][1]
    if probe.get("if") != EXPECTED_SYCL_STEP_IF or config.get("if") != EXPECTED_SYCL_STEP_IF:
        raise AssertionError("AOT probe and configure must run on the SYCL leg")
    if (
        'python scripts\\ci\\ocloc_aot_targets.py ocloc --expect-refused "%WINDOWS_AOT_EXPECT_REFUSED%"'
        ' --github-output "%GITHUB_OUTPUT%"'
    ) not in str(probe.get("run", "")):
        raise AssertionError("AOT probe script missing")
    if probe.get("env", {}).get("WINDOWS_AOT_EXPECT_REFUSED") != (
        "${{ steps.windows_oneapi.outputs.expected_refused }}"
    ):
        raise AssertionError("AOT probe must use the installed oneAPI refused targets")
    if f"-Dsycl_icpx_aot_targets={AOT_OUTPUT}" not in str(config.get("run", "")):
        raise AssertionError("SYCL configure must use AOT probe output")


def _validate_tester_workflow(workflow_text: str) -> None:
    steps = _job_steps(_load_job(workflow_text, "build"))
    installer = _step_index(steps, TESTER_ONEAPI_STEP)
    _validate_no_inline_pin(workflow_text)
    _validate_config_loader(steps, installer, TESTER_SYCL_IF)
    run = str(steps[installer].get("run", ""))
    arguments = run.partition(
        "Start-Process -FilePath $boot -Wait -PassThru -NoNewWindow -ArgumentList @(\n"
    )[2].partition('"--log-dir=$env:RUNNER_TEMP")')[0]
    if not any(
        f"'{SKIP_VS2026}'" in line and not line.lstrip().startswith("#")
        for line in arguments.splitlines()[:MAX_SCRIPT_LINES]
    ):
        raise AssertionError(f"tester bootstrapper must pass {SKIP_VS2026}")
    _validate_tester_aot_probe(steps, installer)


def _validate_tester_aot_probe(steps: list[dict[str, Any]], installer: int) -> None:
    probe_index = _step_index(steps, "aot_targets", key="id")
    build_index = _step_index(steps, TESTER_BUILD_STEP)
    if not installer < probe_index < build_index:
        raise AssertionError("tester AOT probe must run after install and before the SYCL build")
    probe, build = steps[probe_index], steps[build_index]
    if probe.get("if") != TESTER_SYCL_IF or probe.get("shell") != "cmd":
        raise AssertionError("tester AOT probe must run in cmd on the SYCL leg")
    command = (
        r"python scripts\ci\ocloc_aot_targets.py ocloc --expect-refused "
        '"%ONEAPI_WINDOWS_AOT_EXPECT_REFUSED%" --github-output "%GITHUB_OUTPUT%"'
    )
    lines = str(probe.get("run", "")).splitlines()
    if lines != [f"{SETVARS} || exit /b 1", f"{command} || exit /b 1"]:
        raise AssertionError(
            "tester AOT probe must initialize oneAPI and use the shared refused set"
        )
    if build.get("env", {}).get("VMAFX_SYCL_AOT_TARGETS") != AOT_OUTPUT:
        raise AssertionError("tester build must use AOT probe output")


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
        planted = text.replace(SKIP_VS2026, "", 1).replace(
            "          webimage_extracted\\bootstrapper.exe",
            f"          rem {SKIP_VS2026}\n" "          webimage_extracted\\bootstrapper.exe",
            1,
        )
        with self.assertRaises(AssertionError):
            validate_windows_sycl_toolset_contract(planted)

    def test_planted_installer_integrity_failures(self) -> None:
        workflow = yaml.safe_load(WORKFLOW_PATH.read_text(encoding="utf-8"))
        for kind in (
            "size",
            "sha256",
            "late_size",
            "late_sha256",
            "curl",
            "exit",
            "length",
            "refused",
        ):
            planted = deepcopy(workflow)
            target = next(
                s
                for s in planted["jobs"][WINDOWS_GPU_JOB_ID]["steps"]
                if s.get("name") == ONEAPI_STEP_NAME
            )
            if kind in ("size", "sha256"):
                target["run"] = target["run"].replace("INTEL_BASEKIT_WINDOWS_" + kind.upper(), "")
            elif kind == "curl":
                target["run"] = target["run"].replace("--fail", "", 1)
            elif kind == "exit":
                target["run"] = target["run"].replace("if errorlevel 1 exit /b 1", "rem ignored", 1)
            elif kind == "length":
                target["run"] += "\nrem extra" * MAX_SCRIPT_LINES
            elif kind == "refused":
                target["run"] = target["run"].replace("%ONEAPI_WINDOWS_AOT_EXPECT_REFUSED%", "")
            else:
                marker = "INTEL_BASEKIT_WINDOWS_SIZE" if kind == "late_size" else "Get-FileHash"
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
        text = BUILD_CONFIG_PATH.read_text(encoding="utf-8")
        for planted in ("tgllp", EXPECTED_AOT_REFUSED + ",bmg-g31", ""):
            with self.subTest(planted=planted), self.assertRaises(AssertionError):
                _validate_build_config(text.replace(EXPECTED_AOT_REFUSED, planted))

    def test_planted_probe_contract_failures(self) -> None:
        job = _load_job(WORKFLOW_PATH.read_text(encoding="utf-8"))
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
            f"-Dsycl_icpx_aot_targets={AOT_OUTPUT}", ""
        )
        for planted in (missing, late, no_output):
            with self.assertRaises(AssertionError):
                _validate_aot_probe(planted)

    def test_windows_pin_documentation(self) -> None:
        for path in ("build-config.env", "docs/development/tester-image.md"):
            _validate_shared_pin_documentation((REPO_ROOT / path).read_text(encoding="utf-8"))

    def test_planted_missing_shared_pin_documentation_rejected(self) -> None:
        for path in ("build-config.env", "docs/development/tester-image.md"):
            text = (REPO_ROOT / path).read_text(encoding="utf-8")
            for marker in (
                "Windows MSVC+SYCL",
                "Windows SYCL tester zip",
                EXPECTED_ONEAPI_VERSION,
                "ADR-3568",
            ):
                with (
                    self.subTest(path=path, marker=marker),
                    self.assertRaisesRegex(AssertionError, "shared Windows pin documentation"),
                ):
                    _validate_shared_pin_documentation(text.replace(marker, ""))

    def test_shared_windows_pin_values(self) -> None:
        _validate_build_config(BUILD_CONFIG_PATH.read_text(encoding="utf-8"))

    def test_planted_second_build_config_pin_rejected(self) -> None:
        text = BUILD_CONFIG_PATH.read_text(encoding="utf-8")
        planted = f"{text}\nONEAPI_WINDOWS_VERSION=2025.3.0.372\n"
        with self.assertRaisesRegex(AssertionError, "ONEAPI_WINDOWS_VERSION must be pinned once"):
            _validate_build_config(planted)

    def test_planted_second_inline_pin_rejected(self) -> None:
        workflow = yaml.safe_load(WORKFLOW_PATH.read_text(encoding="utf-8"))
        steps = workflow["jobs"][WINDOWS_GPU_JOB_ID]["steps"]
        installer = steps[_step_index(steps, ONEAPI_STEP_NAME)]
        for key, value in WINDOWS_PINS.items():
            with (
                self.subTest(key=key),
                self.assertRaisesRegex(AssertionError, "must come from build-config.env"),
            ):
                installer["env"][key] = value
                try:
                    validate_windows_sycl_toolset_contract(yaml.safe_dump(workflow))
                finally:
                    del installer["env"][key]

    def test_planted_late_config_loader_rejected(self) -> None:
        workflow = yaml.safe_load(WORKFLOW_PATH.read_text(encoding="utf-8"))
        steps = workflow["jobs"][WINDOWS_GPU_JOB_ID]["steps"]
        loader = steps.pop(_step_index(steps, "Load build config"))
        steps.insert(_step_index(steps, ONEAPI_STEP_NAME) + 1, loader)
        with self.assertRaisesRegex(AssertionError, "must precede oneAPI install"):
            validate_windows_sycl_toolset_contract(yaml.safe_dump(workflow))

    def test_tester_workflow_uses_the_shared_pin_and_aot_probe(self) -> None:
        _validate_tester_workflow(TESTER_WORKFLOW_PATH.read_text(encoding="utf-8"))

    def test_planted_tester_switch_only_in_comment_rejected(self) -> None:
        text = TESTER_WORKFLOW_PATH.read_text(encoding="utf-8")
        planted = text.replace(f"'{SKIP_VS2026}',", "").replace(
            '            "--log-dir=$env:RUNNER_TEMP")',
            f"            # '{SKIP_VS2026}',\n" '            "--log-dir=$env:RUNNER_TEMP")',
        )
        with self.assertRaisesRegex(AssertionError, "tester bootstrapper must pass"):
            _validate_tester_workflow(planted)

    def test_planted_tester_aot_contract_failures(self) -> None:
        workflow = yaml.safe_load(TESTER_WORKFLOW_PATH.read_text(encoding="utf-8"))
        for kind in ("late", "missing_refused_set", "missing_output"):
            planted = deepcopy(workflow)
            steps = planted["jobs"]["build"]["steps"]
            probe = _step_index(steps, "aot_targets", key="id")
            build = _step_index(steps, TESTER_BUILD_STEP)
            if kind == "late":
                steps[probe], steps[build] = steps[build], steps[probe]
            elif kind == "missing_refused_set":
                steps[probe]["run"] = steps[probe]["run"].replace(
                    "%ONEAPI_WINDOWS_AOT_EXPECT_REFUSED%", ""
                )
            else:
                del steps[build]["env"]["VMAFX_SYCL_AOT_TARGETS"]
            with self.subTest(kind=kind), self.assertRaisesRegex(AssertionError, "tester"):
                _validate_tester_workflow(yaml.safe_dump(planted))

    def test_contract_registered_in_tooling_suite(self) -> None:
        sys.path.insert(0, str(REPO_ROOT))
        from scripts.ci.suite_registry import load_registry, suite_files  # noqa: PLC0415

        rel_path = Path(__file__).resolve().relative_to(REPO_ROOT).as_posix()
        registry = load_registry(REPO_ROOT)
        self.assertEqual(suite_files(registry, [rel_path], "tooling"), [rel_path])

    def test_negative_planted_old_2025_3_0_pin_rejected(self) -> None:
        config_text = BUILD_CONFIG_PATH.read_text(encoding="utf-8")
        planted = config_text.replace(EXPECTED_WINDOWS_BASEKIT_URL, OLD_WINDOWS_BASEKIT_URL)
        with self.assertRaises(AssertionError) as ctx:
            _validate_build_config(planted)
        self.assertIn("oneAPI >= 2026", str(ctx.exception))

    def test_negative_planted_missing_url_pin_rejected(self) -> None:
        config_text = BUILD_CONFIG_PATH.read_text(encoding="utf-8")
        planted = re.sub(r"(?m)^INTEL_BASEKIT_WINDOWS_URL=.*\n", "", config_text)
        with self.assertRaises(AssertionError) as ctx:
            _validate_build_config(planted)
        self.assertIn("missing INTEL_BASEKIT_WINDOWS_URL pin", str(ctx.exception))

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
            f"          - backend: cuda\n            INTEL_BASEKIT_WINDOWS_URL: {EXPECTED_WINDOWS_BASEKIT_URL}\n",
            1,
        ).replace(EXPECTED_WINDOWS_BASEKIT_URL, OLD_WINDOWS_BASEKIT_URL)
        with self.assertRaises(AssertionError) as ctx2:
            validate_windows_sycl_toolset_contract(cuda_matrix_pin)
        self.assertIn("CUDA matrix row", str(ctx2.exception))


if __name__ == "__main__":
    unittest.main()
